/******************************************************************************
 *
 * Purpose:  Block directory API.
 *
 ******************************************************************************
 * Copyright (c) 2011
 * PCI Geomatics, 90 Allstate Parkway, Markham, Ontario, Canada.
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef PCIDSK_BLOCK_LAYER_H
#define PCIDSK_BLOCK_LAYER_H

#include "blockdir/blockdir.h"

namespace PCIDSK
{

/// The block layer type enumeration.
enum BlockLayerType
{
    /// The free block layer type.
    BLTFree = 0,
    /// The dead block layer type.
    BLTDead = 1,
    /// The image block layer type.
    BLTImage = 2
};

/************************************************************************/
/*                             class BlockLayer                         */
/************************************************************************/

/**
 * Class used as the base class for all block layers.
 *
 * @see BlockDir
 */
class PCIDSK_DLL BlockLayer
{
protected:
    /// The associated block directory.
    BlockDir *          mpoBlockDir;

    /// The block layer index.
    uint32              mnLayer;

    /// The block info list.
    BlockInfoList       moBlockList;

    void                PushBlocks(const BlockInfoList & oBlockList);
    BlockInfoList       PopBlocks(uint32 nBlockCount);

    // We need the block directory interface class to be friend
    // since it needs to push/pop blocks to/from the block layer.
    friend class BlockDir;

/**
 * Sets the type of the layer.
 *
 * @param nLayerType The type of the layer.
 */
    virtual void        _SetLayerType(uint16 nLayerType) = 0;

/**
 * Sets the number of blocks in the block layer.
 *
 * @param nBlockCount The number of blocks in the block layer.
 */
    virtual void        _SetBlockCount(uint32 nBlockCount) = 0;

/**
 * Sets the size in bytes of the layer.
 *
 * @param nLayerSize The size in bytes of the layer.
 */
    virtual void        _SetLayerSize(uint64 nLayerSize) = 0;

    BlockInfo *         GetBlockInfo(uint32 iBlock);

    void                AllocateBlocks(uint64 nOffset, uint64 nSize);

    bool                AreBlocksAllocated(uint64 nOffset, uint64 nSize);

    uint32              GetContiguousCount(uint64 nOffset, uint64 nSize);

    void                FreeBlocks(uint64 nOffset, uint64 nSize);

public:
    BlockLayer(BlockDir * poBlockDir, uint32 nLayer);

    virtual             ~BlockLayer(void);

    void                WriteToLayer(const void * pData,
                                     uint64 nOffset, uint64 nSize);

    bool                ReadFromLayer(void * pData,
                                      uint64 nOffset, uint64 nSize);

    BlockFile *         GetFile(void) const;

    bool                NeedsSwap(void) const;

    bool                IsValid(void) const;

    void                Resize(uint64 nLayerSize);

/**
 * Gets the type of the layer.
 *
 * @return The type of the layer.
 */
    virtual uint16      GetLayerType(void) const = 0;

/**
 * Gets the number of blocks in the block layer.
 *
 * @return The number of blocks in the block layer.
 */
    virtual uint32      GetBlockCount(void) const = 0;

/**
 * Gets the size in bytes of the layer.
 *
 * @return The size in bytes of the layer.
 */
    virtual uint64      GetLayerSize(void) const = 0;
};

} // namespace PCIDSK

#endif
