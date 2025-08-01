/******************************************************************************
 *
 * Project:  KML Driver
 * Purpose:  Class for reading, parsing and handling a kmlfile.
 * Author:   Jens Oberender, j.obi@troja.net
 *
 ******************************************************************************
 * Copyright (c) 2007, Jens Oberender
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/
#include "kmlnode.h"
#include "kml.h"

#include <cstring>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>

#include "cpl_conv.h"
#include "cpl_error.h"
#ifdef HAVE_EXPAT
#include "expat.h"
#endif

constexpr int PARSER_BUF_SIZE = 8192;

KML::KML() = default;

KML::~KML()
{
    if (nullptr != pKMLFile_)
        VSIFCloseL(pKMLFile_);
    CPLFree(papoLayers_);

    delete poTrunk_;
}

bool KML::open(const char *pszFilename)
{
    if (nullptr != pKMLFile_)
        VSIFCloseL(pKMLFile_);

    pKMLFile_ = VSIFOpenL(pszFilename, "r");
    return pKMLFile_ != nullptr;
}

bool KML::parse()
{
    if (nullptr == pKMLFile_)
    {
        sError_ = "No file given";
        return false;
    }

    if (poTrunk_ != nullptr)
    {
        delete poTrunk_;
        poTrunk_ = nullptr;
    }

    if (poCurrent_ != nullptr)
    {
        delete poCurrent_;
        poCurrent_ = nullptr;
    }

    XML_Parser oParser = OGRCreateExpatXMLParser();
    XML_SetUserData(oParser, this);
    XML_SetElementHandler(oParser, startElement, endElement);
    XML_SetCharacterDataHandler(oParser, dataHandler);
    oCurrentParser = oParser;
    nWithoutEventCounter = 0;

    int nDone = 0;
    unsigned nLen = 0;
    std::vector<char> aBuf(PARSER_BUF_SIZE);
    bool bError = false;

    do
    {
        nDataHandlerCounter = 0;
        nLen = static_cast<unsigned>(
            VSIFReadL(aBuf.data(), 1, aBuf.size(), pKMLFile_));
        nDone = nLen < aBuf.size();
        if (XML_Parse(oParser, aBuf.data(), nLen, nDone) == XML_STATUS_ERROR)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "XML parsing of KML file failed : %s at line %d, "
                     "column %d",
                     XML_ErrorString(XML_GetErrorCode(oParser)),
                     static_cast<int>(XML_GetCurrentLineNumber(oParser)),
                     static_cast<int>(XML_GetCurrentColumnNumber(oParser)));
            bError = true;
            break;
        }
        nWithoutEventCounter++;
    } while (!nDone && nLen > 0 && nWithoutEventCounter < 10);

    XML_ParserFree(oParser);
    VSIRewindL(pKMLFile_);

    if (nWithoutEventCounter == 10)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Too much data inside one element. File probably corrupted");
        bError = true;
    }

    if (bError)
    {
        if (poCurrent_ != nullptr)
        {
            while (poCurrent_)
            {
                KMLNode *poTemp = poCurrent_->getParent();
                delete poCurrent_;
                poCurrent_ = poTemp;
            }
            // No need to destroy poTrunk_ : it has been destroyed in
            // the last iteration
        }
        else
        {
            // Case of invalid content after closing element matching
            // first <kml> element
            delete poTrunk_;
        }
        poTrunk_ = nullptr;
        return false;
    }

    poCurrent_ = nullptr;
    return true;
}

void KML::checkValidity()
{
    if (poTrunk_ != nullptr)
    {
        delete poTrunk_;
        poTrunk_ = nullptr;
    }

    if (poCurrent_ != nullptr)
    {
        delete poCurrent_;
        poCurrent_ = nullptr;
    }

    if (pKMLFile_ == nullptr)
    {
        sError_ = "No file given";
        return;
    }

    XML_Parser oParser = OGRCreateExpatXMLParser();
    XML_SetUserData(oParser, this);
    XML_SetElementHandler(oParser, startElementValidate, nullptr);
    XML_SetCharacterDataHandler(oParser, dataHandlerValidate);
    int nCount = 0;

    oCurrentParser = oParser;

    int nDone = 0;
    unsigned nLen = 0;
    std::vector<char> aBuf(PARSER_BUF_SIZE);

    // Parses the file until we find the first element.
    do
    {
        nDataHandlerCounter = 0;
        nLen = static_cast<unsigned>(
            VSIFReadL(aBuf.data(), 1, aBuf.size(), pKMLFile_));
        nDone = nLen < aBuf.size();
        if (XML_Parse(oParser, aBuf.data(), nLen, nDone) == XML_STATUS_ERROR)
        {
            if (nLen <= PARSER_BUF_SIZE - 1)
                aBuf[nLen] = 0;
            else
                aBuf[PARSER_BUF_SIZE - 1] = 0;
            if (strstr(aBuf.data(), "<?xml") &&
                (strstr(aBuf.data(), "<kml") ||
                 (strstr(aBuf.data(), "<Document") &&
                  strstr(aBuf.data(), "/kml/2."))))
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "XML parsing of KML file failed : %s at line %d, column %d",
                    XML_ErrorString(XML_GetErrorCode(oParser)),
                    static_cast<int>(XML_GetCurrentLineNumber(oParser)),
                    static_cast<int>(XML_GetCurrentColumnNumber(oParser)));
            }

            validity = KML_VALIDITY_INVALID;
            XML_ParserFree(oParser);
            VSIRewindL(pKMLFile_);
            return;
        }

        nCount++;
        /* After reading 50 * PARSER_BUF_SIZE bytes, and not finding whether the file */
        /* is KML or not, we give up and fail silently */
    } while (!nDone && nLen > 0 && validity == KML_VALIDITY_UNKNOWN &&
             nCount < 50);

    XML_ParserFree(oParser);
    VSIRewindL(pKMLFile_);
    poCurrent_ = nullptr;
}

void XMLCALL KML::startElement(void *pUserData, const char *pszName,
                               const char **ppszAttr)
{
    KML *poKML = static_cast<KML *>(pUserData);
    try
    {
        poKML->nWithoutEventCounter = 0;

        const char *pszColumn = strchr(pszName, ':');
        if (pszColumn)
            pszName = pszColumn + 1;

        if (poKML->poTrunk_ == nullptr ||
            (poKML->poCurrent_ != nullptr &&
             poKML->poCurrent_->getName().compare("description") != 0))
        {
            if (poKML->nDepth_ == 1024)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Too big depth level (%d) while parsing KML.",
                         poKML->nDepth_);
                XML_StopParser(poKML->oCurrentParser, XML_FALSE);
                return;
            }

            KMLNode *poMynew = new KMLNode();
            poMynew->setName(pszName);
            poMynew->setLevel(poKML->nDepth_);

            for (int i = 0; ppszAttr[i]; i += 2)
            {
                Attribute *poAtt = new Attribute();
                poAtt->sName = ppszAttr[i];
                poAtt->sValue = ppszAttr[i + 1];
                poMynew->addAttribute(poAtt);
            }

            if (poKML->poTrunk_ == nullptr)
                poKML->poTrunk_ = poMynew;
            if (poKML->poCurrent_ != nullptr)
                poMynew->setParent(poKML->poCurrent_);
            poKML->poCurrent_ = poMynew;

            poKML->nDepth_++;
        }
        else if (poKML->poCurrent_ != nullptr)
        {
            std::string sNewContent = "<";
            sNewContent += pszName;
            for (int i = 0; ppszAttr[i]; i += 2)
            {
                sNewContent += " ";
                sNewContent += ppszAttr[i];
                sNewContent += "=\"";
                sNewContent += ppszAttr[i + 1];
                sNewContent += "\"";
            }
            sNewContent += ">";
            if (poKML->poCurrent_->numContent() == 0)
                poKML->poCurrent_->addContent(sNewContent);
            else
                poKML->poCurrent_->appendContent(sNewContent);
        }
    }
    catch (const std::exception &ex)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "KML: libstdc++ exception : %s",
                 ex.what());
        XML_StopParser(poKML->oCurrentParser, XML_FALSE);
    }
}

void XMLCALL KML::startElementValidate(void *pUserData, const char *pszName,
                                       const char **ppszAttr)
{
    KML *poKML = static_cast<KML *>(pUserData);

    if (poKML->validity != KML_VALIDITY_UNKNOWN)
        return;

    poKML->validity = KML_VALIDITY_INVALID;

    const char *pszColumn = strchr(pszName, ':');
    if (pszColumn)
        pszName = pszColumn + 1;

    if (strcmp(pszName, "kml") == 0 || strcmp(pszName, "Document") == 0)
    {
        // Check all Attributes
        for (int i = 0; ppszAttr[i]; i += 2)
        {
            // Find the namespace and determine the KML version
            if (strcmp(ppszAttr[i], "xmlns") == 0)
            {
                // Is it KML 2.2?
                if ((strcmp(ppszAttr[i + 1],
                            "http://earth.google.com/kml/2.2") == 0) ||
                    (strcmp(ppszAttr[i + 1],
                            "http://www.opengis.net/kml/2.2") == 0))
                {
                    poKML->validity = KML_VALIDITY_VALID;
                    poKML->sVersion_ = "2.2";
                }
                else if (strcmp(ppszAttr[i + 1],
                                "http://earth.google.com/kml/2.1") == 0)
                {
                    poKML->validity = KML_VALIDITY_VALID;
                    poKML->sVersion_ = "2.1";
                }
                else if (strcmp(ppszAttr[i + 1],
                                "http://earth.google.com/kml/2.0") == 0)
                {
                    poKML->validity = KML_VALIDITY_VALID;
                    poKML->sVersion_ = "2.0";
                }
                else
                {
                    CPLDebug("KML",
                             "Unhandled xmlns value : %s. Going on though...",
                             ppszAttr[i]);
                    poKML->validity = KML_VALIDITY_VALID;
                    poKML->sVersion_ = "?";
                }
            }
        }

        if (poKML->validity == KML_VALIDITY_INVALID)
        {
            CPLDebug("KML", "Did not find xmlns attribute in <kml> element. "
                            "Going on though...");
            poKML->validity = KML_VALIDITY_VALID;
            poKML->sVersion_ = "?";
        }
    }
}

void XMLCALL KML::dataHandlerValidate(void *pUserData,
                                      const char * /* pszData */,
                                      int /* nLen */)
{
    KML *poKML = static_cast<KML *>(pUserData);

    poKML->nDataHandlerCounter++;
    if (poKML->nDataHandlerCounter >= PARSER_BUF_SIZE)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "File probably corrupted (million laugh pattern)");
        XML_StopParser(poKML->oCurrentParser, XML_FALSE);
    }
}

void XMLCALL KML::endElement(void *pUserData, const char *pszName)
{
    KML *poKML = static_cast<KML *>(pUserData);

    try
    {
        poKML->nWithoutEventCounter = 0;

        const char *pszColumn = strchr(pszName, ':');
        if (pszColumn)
            pszName = pszColumn + 1;

        if (poKML->poCurrent_ != nullptr &&
            poKML->poCurrent_->getName().compare(pszName) == 0)
        {
            poKML->nDepth_--;
            KMLNode *poTmp = poKML->poCurrent_;
            // Split the coordinates
            if (poKML->poCurrent_->getName().compare("coordinates") == 0 &&
                poKML->poCurrent_->numContent() == 1)
            {
                const std::string sData = poKML->poCurrent_->getContent(0);
                std::size_t nPos = 0;
                const std::size_t nLength = sData.length();
                const char *pszData = sData.c_str();
                while (true)
                {
                    // Cut off whitespaces
                    while (nPos < nLength &&
                           (pszData[nPos] == ' ' || pszData[nPos] == '\n' ||
                            pszData[nPos] == '\r' || pszData[nPos] == '\t'))
                        nPos++;

                    if (nPos == nLength)
                        break;

                    const std::size_t nPosBegin = nPos;
                    size_t nContentSize = 0;

                    // Get content
                    while (nPos < nLength && pszData[nPos] != ' ' &&
                           pszData[nPos] != '\n' && pszData[nPos] != '\r' &&
                           pszData[nPos] != '\t')
                    {
                        nContentSize++;
                        nPos++;
                    }

                    if (nContentSize > 0)
                    {
                        std::string sTmp(pszData + nPosBegin, nContentSize);
                        poKML->poCurrent_->addContent(sTmp);
                    }
                }
                if (poKML->poCurrent_->numContent() > 1)
                    poKML->poCurrent_->deleteContent(0);
            }
            else if (poKML->poCurrent_->numContent() == 1)
            {
                const std::string sData = poKML->poCurrent_->getContent(0);
                std::string sDataWithoutNL;
                std::size_t nPos = 0;
                const std::size_t nLength = sData.length();
                const char *pszData = sData.c_str();
                std::size_t nLineStartPos = 0;
                bool bLineStart = true;

                // Re-assemble multi-line content by removing leading spaces for
                // each line.  I am not sure why we do that. Should we preserve
                // content as such?
                while (nPos < nLength)
                {
                    const char ch = pszData[nPos];
                    if (bLineStart &&
                        (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'))
                        nLineStartPos++;
                    else if (ch == '\n' || ch == '\r')
                    {
                        if (!bLineStart)
                        {
                            std::string sTmp(pszData + nLineStartPos,
                                             nPos - nLineStartPos);
                            if (!sDataWithoutNL.empty())
                                sDataWithoutNL += '\n';
                            sDataWithoutNL += sTmp;
                            bLineStart = true;
                        }
                        nLineStartPos = nPos + 1;
                    }
                    else
                    {
                        bLineStart = false;
                    }
                    nPos++;
                }

                if (nLineStartPos > 0)
                {
                    if (nLineStartPos < nPos)
                    {
                        std::string sTmp(pszData + nLineStartPos,
                                         nPos - nLineStartPos);
                        if (!sDataWithoutNL.empty())
                            sDataWithoutNL += '\n';
                        sDataWithoutNL += sTmp;
                    }

                    poKML->poCurrent_->deleteContent(0);
                    poKML->poCurrent_->addContent(sDataWithoutNL);
                }
            }

            if (poKML->poCurrent_->getParent() != nullptr)
                poKML->poCurrent_ = poKML->poCurrent_->getParent();
            else
                poKML->poCurrent_ = nullptr;

            if (!poKML->isHandled(pszName))
            {
                CPLDebug("KML", "Not handled: %s", pszName);
                delete poTmp;
                if (poKML->poCurrent_ == poTmp)
                    poKML->poCurrent_ = nullptr;
                if (poKML->poTrunk_ == poTmp)
                    poKML->poTrunk_ = nullptr;
            }
            else
            {
                if (poKML->poCurrent_ != nullptr)
                    poKML->poCurrent_->addChildren(poTmp);
            }
        }
        else if (poKML->poCurrent_ != nullptr)
        {
            std::string sNewContent = "</";
            sNewContent += pszName;
            sNewContent += ">";
            if (poKML->poCurrent_->numContent() == 0)
                poKML->poCurrent_->addContent(sNewContent);
            else
                poKML->poCurrent_->appendContent(sNewContent);
        }
    }
    catch (const std::exception &ex)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "KML: libstdc++ exception : %s",
                 ex.what());
        XML_StopParser(poKML->oCurrentParser, XML_FALSE);
    }
}

void XMLCALL KML::dataHandler(void *pUserData, const char *pszData, int nLen)
{
    KML *poKML = static_cast<KML *>(pUserData);

    poKML->nWithoutEventCounter = 0;

    if (nLen < 1 || poKML->poCurrent_ == nullptr)
        return;

    poKML->nDataHandlerCounter++;
    if (poKML->nDataHandlerCounter >= PARSER_BUF_SIZE)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "File probably corrupted (million laugh pattern)");
        XML_StopParser(poKML->oCurrentParser, XML_FALSE);
    }

    try
    {
        std::string sData(pszData, nLen);

        if (poKML->poCurrent_->numContent() == 0)
            poKML->poCurrent_->addContent(sData);
        else
            poKML->poCurrent_->appendContent(sData);
    }
    catch (const std::exception &ex)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "KML: libstdc++ exception : %s",
                 ex.what());
        XML_StopParser(poKML->oCurrentParser, XML_FALSE);
    }
}

bool KML::isValid()
{
    checkValidity();

    if (validity == KML_VALIDITY_VALID)
        CPLDebug("KML", "Valid: 1 Version: %s", sVersion_.c_str());

    return validity == KML_VALIDITY_VALID;
}

std::string KML::getError() const
{
    return sError_;
}

int KML::classifyNodes()
{
    if (poTrunk_ == nullptr)
        return false;
    return poTrunk_->classify(this);
}

void KML::eliminateEmpty()
{
    if (poTrunk_ != nullptr)
        poTrunk_->eliminateEmpty(this);
}

void KML::print(unsigned short nNum)
{
    if (poTrunk_ != nullptr)
        poTrunk_->print(nNum);
}

bool KML::isHandled(std::string const &elem) const
{
    return isLeaf(elem) || isFeature(elem) || isFeatureContainer(elem) ||
           isContainer(elem) || isRest(elem);
}

bool KML::isLeaf(std::string const & /* elem */) const
{
    return false;
}

bool KML::isFeature(std::string const & /* elem */) const
{
    return false;
}

bool KML::isFeatureContainer(std::string const & /* elem */) const
{
    return false;
}

bool KML::isContainer(std::string const & /* elem */) const
{
    return false;
}

bool KML::isRest(std::string const & /* elem */) const
{
    return false;
}

void KML::findLayers(KMLNode * /* poNode */, int /* bKeepEmptyContainers */)
{
    // idle
}

bool KML::hasOnlyEmpty() const
{
    return poTrunk_->hasOnlyEmpty();
}

int KML::getNumLayers() const
{
    return nNumLayers_;
}

bool KML::selectLayer(int nNum)
{
    if (nNumLayers_ < 1 || nNum >= nNumLayers_)
        return false;
    poCurrent_ = papoLayers_[nNum];
    return true;
}

std::string KML::getCurrentName() const
{
    std::string tmp;
    if (poCurrent_ != nullptr)
    {
        tmp = poCurrent_->getNameElement();
    }
    return tmp;
}

Nodetype KML::getCurrentType() const
{
    if (poCurrent_ != nullptr)
        return poCurrent_->getType();

    return Unknown;
}

int KML::is25D() const
{
    if (poCurrent_ != nullptr)
        return poCurrent_->is25D();

    return Unknown;
}

int KML::getNumFeatures()
{
    if (poCurrent_ == nullptr)
        return -1;

    return static_cast<int>(poCurrent_->getNumFeatures());
}

Feature *KML::getFeature(std::size_t nNum, int &nLastAsked, int &nLastCount)
{
    if (poCurrent_ == nullptr)
        return nullptr;

    return poCurrent_->getFeature(nNum, nLastAsked, nLastCount);
}

void KML::unregisterLayerIfMatchingThisNode(KMLNode *poNode)
{
    for (int i = 0; i < nNumLayers_;)
    {
        if (papoLayers_[i] == poNode)
        {
            if (i < nNumLayers_ - 1)
            {
                memmove(papoLayers_ + i, papoLayers_ + i + 1,
                        (nNumLayers_ - 1 - i) * sizeof(KMLNode *));
            }
            nNumLayers_--;
            break;
        }
        i++;
    }
}
