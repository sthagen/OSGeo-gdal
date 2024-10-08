/******************************************************************************
 *
 * Purpose: Support for reading and manipulating PCIDSK RPC Segments
 *
 ******************************************************************************
 * Copyright (c) 2009
 * PCI Geomatics, 90 Allstate Parkway, Markham, Ontario, Canada.
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/
#ifndef INCLUDE_PCIDSK_SEGMENT_PCIDSKRPCMODEL_H
#define INCLUDE_PCIDSK_SEGMENT_PCIDSKRPCMODEL_H

#include "pcidsk_rpc.h"
#include "segment/cpcidsksegment.h"

namespace PCIDSK {
    class PCIDSKFile;

    class CPCIDSKRPCModelSegment : virtual public PCIDSKRPCSegment,
                                   public CPCIDSKSegment
    {
    public:
        CPCIDSKRPCModelSegment(PCIDSKFile *file, int segment,const char *segment_pointer);
        ~CPCIDSKRPCModelSegment();

        // Implementation of PCIDSKRPCSegment
        // Get the X and Y RPC coefficients
        std::vector<double> GetXNumerator(void) const override;
        std::vector<double> GetXDenominator(void) const override;
        std::vector<double> GetYNumerator(void) const override;
        std::vector<double> GetYDenominator(void) const override;

        // Set the X and Y RPC Coefficients
        void SetCoefficients(const std::vector<double>& xnum,
            const std::vector<double>& xdenom, const std::vector<double>& ynum,
            const std::vector<double>& ydenom) override;

        // Get the RPC offset/scale Coefficients
        void GetRPCTranslationCoeffs(double& xoffset, double& xscale,
            double& yoffset, double& yscale, double& zoffset, double& zscale,
            double& pixoffset, double& pixscale, double& lineoffset, double& linescale) const override;

        // Set the RPC offset/scale Coefficients
        void SetRPCTranslationCoeffs(
            const double xoffset, const double xscale,
            const double yoffset, const double yscale,
            const double zoffset, const double zscale,
            const double pixoffset, const double pixscale,
            const double lineoffset, const double linescale) override;

        // Get the adjusted X values
        std::vector<double> GetAdjXValues(void) const override;
        // Get the adjusted Y values
        std::vector<double> GetAdjYValues(void) const override;

        // Set the adjusted X/Y values
        void SetAdjCoordValues(const std::vector<double>& xcoord,
            const std::vector<double>& ycoord) override;

        // Get whether or not this is a user-generated RPC model
        bool IsUserGenerated(void) const override;
        // Set whether or not this is a user-generated RPC model
        void SetUserGenerated(bool usergen) override;

        // Get whether the model has been adjusted
        bool IsNominalModel(void) const override;
        // Set whether the model has been adjusted
        void SetIsNominalModel(bool nominal) override;

        // Get sensor name
        std::string GetSensorName(void) const override;
        // Set sensor name
        void SetSensorName(const std::string& name) override;

        // Output projection information of RPC Model
        void GetMapUnits(std::string& map_units, std::string& proj_parms) const override;

        // Set the map units
        void SetMapUnits(std::string const& map_units, std::string const& proj_parms) override;

        // Get the number of lines
        unsigned int GetLines(void) const override;

        // Get the number of pixels
        unsigned int GetPixels(void) const override;

        // Set the number of lines/pixels
        void SetRasterSize(const unsigned int lines, const unsigned int pixels) override;

        // Set the downsample factor
        void SetDownsample(const unsigned int downsample) override;

        // Get the downsample factor
        unsigned int GetDownsample(void) const override;

        //synchronize the segment on disk.
        void Synchronize() override;
    private:
        // Helper housekeeping functions
        void Load();
        void Write();

        // Struct to store details of the RPC model
        struct PCIDSKRPCInfo
        {
            bool userrpc; // whether or not the RPC was generated from GCPs
            bool adjusted; // Whether or not the RPC has been adjusted
            int downsample; // Epipolar Downsample factor

            unsigned int pixels; // pixels in the image
            unsigned int lines; // lines in the image

            unsigned int num_coeffs; // number of coefficientsg

            std::vector<double> pixel_num; // numerator, pixel direction
            std::vector<double> pixel_denom; // denominator, pixel direction
            std::vector<double> line_num; // numerator, line direction
            std::vector<double> line_denom; // denominator, line direction

            // Scale/offset coefficients in the ground domain
            double x_off;
            double x_scale;

            double y_off;
            double y_scale;

            double z_off;
            double z_scale;

            // Scale/offset coefficients in the raster domain
            double pix_off;
            double pix_scale;

            double line_off;
            double line_scale;

            std::vector<double> x_adj; // adjusted X values
            std::vector<double> y_adj; // adjusted Y values

            std::string sensor_name; // the name of the sensor

            std::string map_units; // the map units string
            std::string proj_parms; // Projection parameters encoded as text

            // TODO: Projection Info

            // The raw segment data
            PCIDSKBuffer seg_data;
        };

        PCIDSKRPCInfo *pimpl_;
        bool loaded_;
        bool mbModified;

        //this member is used when the segment was newly created
        //and nothing was yet set in it.
        bool mbEmpty;
    };
}

#endif // INCLUDE_PCIDSK_SEGMENT_PCIDSKRPCMODEL_H
