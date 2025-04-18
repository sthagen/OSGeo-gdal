/******************************************************************************
 *
 * Project:  GDAL algorithms
 * Purpose:  Tests for the marching squares algorithm
 * Author:   Hugo Mercier, <hugo dot mercier at oslandia dot com>
 *
 ******************************************************************************
 * Copyright (c) 2018, Hugo Mercier, <hugo dot mercier at oslandia dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "gdal_unit_test.h"

#include "gdal_alg.h"

#include "marching_squares/level_generator.h"
#include "marching_squares/polygon_ring_appender.h"
#include "marching_squares/segment_merger.h"
#include "marching_squares/contour_generator.h"

#ifdef DEBUG
#include <fstream>
#endif

#include <limits>

#include "gtest_include.h"

namespace marching_squares
{
class TestPolygonWriter
{
  public:
    void startPolygon(double level)
    {
        currentPolygon_ = &polygons_[level];
    }

    void endPolygon()
    {
    }

    void addPart(const std::list<marching_squares::Point> &ring)
    {
        PolygonPart part;
        part.push_back(ring);
        currentPolygon_->emplace_back(part);
        currentPart_ = &currentPolygon_->back();
    }

    void addInteriorRing(const std::list<marching_squares::Point> &ring)
    {
        currentPart_->push_back(ring);
    }

    void out(std::ostream &ostr, double level) const
    {
        auto pIt = polygons_.find(level);
        if (pIt == polygons_.end())
            return;

        for (const auto &part : pIt->second)
        {
            ostr << "{ ";
            for (const auto &ring : part)
            {
                ostr << "{ ";
                for (const auto &pt : ring)
                {
                    ostr << "(" << pt.x << "," << pt.y << ") ";
                }
                ostr << "} ";
            }
            ostr << "} ";
        }
    }

#ifdef DEBUG
    void toSvg(const std::string &filename)
    {
        std::ofstream ofs(filename);
        ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\" "
               "standalone=\"no\"?><svg xmlns=\"http://www.w3.org/2000/svg\" "
               "version=\"1.1\">\n";
        ofs << "<defs><marker id=\"arrow\" refX=\"0\" refY=\"0\" "
               "orient=\"auto\">\n";
        ofs << "<path d=\"M 0,0 L-1.5,-1 L-1.5,1 L0,0\" "
               "style=\"fill:#000000;\" />\n";
        ofs << "</marker></defs>\n";

        const std::string colors[] = {"white", "#bbb", "#888",
                                      "#666",  "#333", "black"};

        int level = 0;
        for (auto &p : polygons_)
        {
            for (const auto &part : p.second)
            {
                ofs << "<path style=\"fill:" << colors[level] << ";\" d=\"";
                for (const auto &ring : part)
                {
                    ofs << "M ";
                    for (const auto &point : ring)
                    {
                        ofs << point.x * 10 << "," << point.y * 10 << " ";
                    }
                }
                ofs << "\"/>";
            }
            level++;
        }
        ofs << "</svg>";
    }
#endif

  private:
    typedef std::vector<LineString> PolygonPart;
    typedef std::vector<PolygonPart> Polygon;
    Polygon *currentPolygon_ = nullptr;
    PolygonPart *currentPart_ = nullptr;

  public:
    std::map<double, Polygon> polygons_;
};

static bool equal_linestrings(const LineString &ls1, const LineString &ls2)
{
    if (ls1.size() != ls2.size())
        return false;
    auto it1 = ls1.begin();
    auto it2 = ls2.begin();
    for (; it1 != ls1.end(); it1++, it2++)
    {
        if (!(*it1 == *it2))
            return false;
    }
    return true;
}
}  // namespace marching_squares

namespace
{
using namespace marching_squares;

// Common fixture with test data
struct test_ms_polygon : public ::testing::Test
{
};

// Dummy test
TEST_F(test_ms_polygon, dummy)
{
    // one pixel
    std::vector<double> data = {2.0};
    TestPolygonWriter w;
    {
        PolygonRingAppender<TestPolygonWriter> appender(w);
        IntervalLevelRangeIterator levels(
            0.0, 10.0, -std::numeric_limits<double>::infinity());
        SegmentMerger<PolygonRingAppender<TestPolygonWriter>,
                      IntervalLevelRangeIterator>
            writer(appender, levels, /* polygonize */ true);
        ContourGenerator<decltype(writer), IntervalLevelRangeIterator> cg(
            1, 1, false, NaN, writer, levels);
        cg.feedLine(&data[0]);
    }

    {
        std::ostringstream ostr;
        w.out(ostr, 10.0);
        // "Polygon #0",
        EXPECT_EQ(ostr.str(), "{ { (0.5,1) (1,1) (1,0.5) (1,0) (0.5,0) (0,0) "
                              "(0,0.5) (0,1) (0.5,1) } } ");
    }
}

TEST_F(test_ms_polygon, four_pixels)
{
    // four pixels
    // two rings
    // 5  10
    // 10  5
    // levels = 0, 10
    //
    // legend:
    //  :   contour
    //  #   border (level 0)
    //  =   border (level 10)
    //
    //   NaN                NaN                NaN
    //    +------------------+------------------+------------------+
    //    |                  |                  |                  |
    //    |    (0,0)         |      (1,0)       |      (2,0)       |
    //    |       5         5|      7.5       10|        10        |
    //    |        +#########+########+########o+========++        |
    //    |        #         |        |         :        ||        |
    //    |        #         |        |         :        ||        |
    //    |        #         |        |         :        ||        |
    //    +--------+---------+--------+---------o........o+--------+
    //    |NaN   5 #        5|                10|      10#      NaN|
    //    |        #         |                  |        #         |
    //    |        #         |                  |        #         |
    //    |    7.5++---------+ 7.5           7.5+--------+         |
    //    |        #         |                  |        #         |
    //    |        #         |                  |        #         |
    //    |        #         |       7.5        |        #         |
    //    +-------++.........o--------+---------+--------+---------+
    //    |NaN  10||       10:        |        5|      5 #      NaN|
    //    |       ||         :        |         |        #         |
    //    |       ||         :        |         |        #         |
    //    |       ++=========o########+#########+########+         |
    //    |      10        10|      7.5        5|        5         |
    //    |     (0,2)        |       (1,2)      |       (2,2)      |
    //    |                  |                  |                  |
    //    +------------------+------------------+------------------+
    //  NaN                 NaN                NaN                NaN

    std::vector<double> data = {5.0, 10.0, 10.0, 5.0};
    TestPolygonWriter w;
    {
        PolygonRingAppender<TestPolygonWriter> appender(w);
        IntervalLevelRangeIterator levels(
            0.0, 10.0, -std::numeric_limits<double>::infinity());
        SegmentMerger<PolygonRingAppender<TestPolygonWriter>,
                      IntervalLevelRangeIterator>
            writer(appender, levels, /* polygonize */ true);
        ContourGenerator<decltype(writer), IntervalLevelRangeIterator> cg(
            2, 2, false, NaN, writer, levels);
        cg.feedLine(&data[0]);
        cg.feedLine(&data[2]);
    }

    {
        std::ostringstream ostr;
        w.out(ostr, 10.0);
        // "Polygon #1"
        EXPECT_EQ(ostr.str(),
                  "{ { (1.5,2) (2,2) (2,1.5) (2,1) (2,0.5) (1.5,0.5) (1.5,0.5) "
                  "(1.5,0) (1,0) (0.5,0) (0,0) (0,0.5) (0,1) (0,1.5) (0.5,1.5) "
                  "(0.5,1.5) (0.5,2) (1,2) (1.5,2) } } ");
    }
    {
        std::ostringstream ostr;
        w.out(ostr, 20.0);
        // "Polygon #2"
        EXPECT_EQ(ostr.str(),
                  "{ { (2,0.5) (2,0.5) (2,0) (1.5,0) (1.5,0) (1.5,0.5) "
                  "(1.5,0.5) (2,0.5) } } { { (0.5,1.5) (0.5,1.5) (0,1.5) "
                  "(0,1.5) (0,2) (0.5,2) (0.5,2) (0.5,1.5) } } ");
    }
}

TEST_F(test_ms_polygon, four_pixels_2)
{
    // four pixels
    // 155    155.01
    // 154.99 155
    // levels = 155

    //   NaN                NaN                NaN
    //    +------------------+------------------+------------------+
    //    |                  |                  |                  |
    //    |    (0,0)         |      (1,0)       |      (2,0)       |
    //    |      155         |     155.005      |      155.01      |
    //    |        +---------+--------+---------+---------+        |
    //    |        |       155        |      155.01       |        |
    //    |        |         |        |         |         |        |
    //    |        |         |     155.005      |         |        |
    //    +--------+---------+--------+---------+---------+--------+
    //    |NaN   155       155               155.01    155.01   NaN|
    //    |        |         |                  |         |        |
    //    |    154.995       |                  |      155.005     |
    //    |        +-------154.995           155.005------+        |
    //    |        |         |                  |         |        |
    //    |        |         |                  |         |        |
    //    |        |         |                  |         |        |
    //    +--------+---------+--------+---------+---------+--------+
    //    |NaN  154.99    154.99   154.995    155       155     NaN|
    //    |        |         |        |         |         |        |
    //    |        |         |        |         |         |        |
    //    |        +---------+--------+---------+---------+        |
    //    |     154.99    154.99   154.995    155       155        |
    //    |     (0,2)        |       (1,2)      |       (2,2)      |
    //    |                  |                  |                  |
    //    +------------------+------------------+------------------+
    //  NaN                 NaN                NaN                NaN

    std::vector<double> data = {155.0, 155.01, 154.99, 155.0};
    {
        TestPolygonWriter w;
        {
            PolygonRingAppender<TestPolygonWriter> appender(w);
            const double levels[] = {155.0};
            FixedLevelRangeIterator levelGenerator(
                levels, 1, -std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::infinity());
            SegmentMerger<PolygonRingAppender<TestPolygonWriter>,
                          FixedLevelRangeIterator>
                writer(appender, levelGenerator, /* polygonize */ true);
            ContourGenerator<decltype(writer), FixedLevelRangeIterator> cg(
                2, 2, false, NaN, writer, levelGenerator);
            cg.feedLine(&data[0]);
            cg.feedLine(&data[2]);
        }
        EXPECT_EQ(w.polygons_.size(), 2);
        {
            std::ostringstream ostr;
            w.out(ostr, 155.0);
            // "Polygon #0"
            EXPECT_EQ(
                ostr.str(),
                "{ { (1.4999,2) (1.4999,1.5) (0.5,0.5001) (0,0.5001) (0,1) "
                "(0,1.5) (0,2) (0.5,2) (1,2) (1.4999,2) } } ");
        }
        {
            std::ostringstream ostr;
            w.out(ostr, Inf);
            // "Polygon #1"
            EXPECT_EQ(
                ostr.str(),
                "{ { (1.5,2) (2,2) (2,1.5) (2,1) (2,0.5) (2,0) (1.5,0) (1,0) "
                "(0.5,0) (0,0) (0,0.5) (0,0.5001) (0.5,0.5001) (1.4999,1.5) "
                "(1.4999,2) (1.5,2) } } ");
        }
    }

    {
        TestPolygonWriter w;
        {
            PolygonRingAppender<TestPolygonWriter> appender(w);
            const double levels[] = {155.0};
            FixedLevelRangeIterator levelGenerator(
                levels, 1, -std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::infinity());
            SegmentMerger<PolygonRingAppender<TestPolygonWriter>,
                          FixedLevelRangeIterator>
                writer(appender, levelGenerator, /* polygonize */ true);
            writer.setSkipLevels({1});
            ContourGenerator<decltype(writer), FixedLevelRangeIterator> cg(
                2, 2, false, NaN, writer, levelGenerator);
            cg.feedLine(&data[0]);
            cg.feedLine(&data[2]);
        }
        {
            EXPECT_EQ(w.polygons_.size(), 2);
            auto iter = w.polygons_.find(Inf);
            ASSERT_TRUE(iter != w.polygons_.end());
            EXPECT_TRUE(iter->second.empty());
        }

        {
            std::ostringstream ostr;
            w.out(ostr, 155.0);
            // "Polygon #0"
            EXPECT_EQ(
                ostr.str(),
                "{ { (1.4999,2) (1.4999,1.5) (0.5,0.5001) (0,0.5001) (0,1) "
                "(0,1.5) (0,2) (0.5,2) (1,2) (1.4999,2) } } ");
        }
    }
}

TEST_F(test_ms_polygon, nine_pixels)
{
    // nine pixels
    // two nested rings
    // levels = 1, 11, 21
    // pixels
    // +-----+-----+-----+-----+-----+
    // |     |     |     |     |     |
    // | NaN | NaN | NaN | NaN | NaN |
    // |     |     |     |     |     |
    // +-----+-----+-----+-----+-----+
    // |     |     |     |     |     |
    // | NaN |  0  |  4  |  0  | NaN |
    // |     |     |     |     |     |
    // +-----+-----+-----+-----+-----+
    // |     |     |     |     |     |
    // | NaN |  4  |  12 |  4  | NaN |
    // |     |     |     |     |     |
    // +-----+-----+-----+-----+-----+
    // |     |     |     |     |     |
    // | NaN |  0  |  4  |  0  | NaN |
    // |     |     |     |     |     |
    // +-----+-----+-----+-----+-----+
    // |     |     |     |     |     |
    // | NaN | NaN | NaN | NaN | NaN |
    // |     |     |     |     |     |
    // +-----+-----+-----+-----+-----+
    //
    //   NaN                NaN                NaN                NaN NaN
    //    +------------------+------------------+------------------+------------------+
    //    |                  |                  |                  | | | (0,0)
    //    |      (1,0)       |      (2,0)       |                  | |        0
    //    0|        2        4|         2       0|         0        | |
    //    +---------+---o----+---------+---------+----o---+---------+        |
    //    |        |         |   :    |         |         |    :   |         | |
    //    |        |         |   :    |         |         |    :   |         | |
    //    |        |         |   :    |         |         |    :   |         | |
    //    +--------+---------+---o----+---------+---------+----o---+---------+--------+
    //    NaN |NaN    0|        0| _/     2        4|         2     \_0| |0 | |
    //    o.........o/                 |                 \o.........o        |
    //    |        |         |                  |                  |         | |
    //    |       2+---------+ 2                |                 2+---------+2
    //    | |        |         |                  |                  |         |
    //    | |        |         |                 _o_                 |         |
    //    | |        |         |                / | \                |         |
    //    |
    //    +--------+---------+---------------o--+--o---------------+---------+--------+
    //    NaN |NaN    4|        4|                \12 /               4| |4 | |
    //    |         |                 -o-                 |         |        |
    //    |        |         |                  |                  |         | |
    //    |       2+---------+ 2                |                 2+---------+2
    //    | |        |         |                  |                  |         |
    //    | |        o.........o_                 |                 _o.........o
    //    | |        |         | \_     2         |        2      _/ |         |
    //    |
    //    +--------+---------+---o----+---------+--------+----o/---+---------+--------+
    //    NaN |NaN    0|        0|   :    |        4|        |    :   0| |0 | |
    //    |         |   :    |         |        |    :    |         |        |
    //    |        |         |   :    |         |        |    :    |         | |
    //    |        +---------+---o----+---------+--------+----o----+---------+ |
    //    |       0         0|        2        4|        2        0|         0 |
    //    |     (0,3)        |       (1,3)      |       (2,3)      | | | | | | |
    //    +------------------+------------------+------------------+------------------+
    //  NaN                 NaN                NaN                NaN NaN
    std::vector<double> data = {0.0, 4.0, 0.0, 4.0, 12.0, 4.0, 0.0, 4.0, 0.0};
    TestPolygonWriter w;
    {
        PolygonRingAppender<TestPolygonWriter> appender(w);
        IntervalLevelRangeIterator levels(
            1.0, 10.0, -std::numeric_limits<double>::infinity());
        SegmentMerger<PolygonRingAppender<TestPolygonWriter>,
                      IntervalLevelRangeIterator>
            writer(appender, levels, /* polygonize */ true);
        ContourGenerator<decltype(writer), IntervalLevelRangeIterator> cg(
            3, 3, false, NaN, writer, levels);
        cg.feedLine(&data[0]);
        cg.feedLine(&data[3]);
        cg.feedLine(&data[6]);
    }

    {
        std::ostringstream ostr;
        w.out(ostr, 1.0);
        // "Polygon #0"
        EXPECT_EQ(ostr.str(),
                  "{ { (0.5,0.75) (0.75,0.5) (0.75,0) (0.5,0) (0,0) (0,0.5) "
                  "(0,0.75) (0.5,0.75) } } { { (2.5,0.75) (3,0.75) (3,0.5) "
                  "(3,0) (2.5,0) (2.25,0) (2.25,0.5) (2.5,0.75) } } { { "
                  "(0.75,3) (0.75,2.5) (0.5,2.25) (0,2.25) (0,2.5) (0,3) "
                  "(0.5,3) (0.75,3) } } { { (2.5,3) (3,3) (3,2.5) (3,2.25) "
                  "(2.5,2.25) (2.25,2.5) (2.25,3) (2.5,3) } } ");
    }
    {
        std::ostringstream ostr;
        w.out(ostr, 11.0);
        // "Polygon #1"
        EXPECT_EQ(ostr.str(),
                  "{ { (2.25,2.5) (2.5,2.25) (3,2.25) (3,2) (3,1.5) (3,1) "
                  "(3,0.75) (2.5,0.75) (2.25,0.5) (2.25,0) (2,0) (1.5,0) (1,0) "
                  "(0.75,0) (0.75,0.5) (0.5,0.75) (0,0.75) (0,1) (0,1.5) (0,2) "
                  "(0,2.25) (0.5,2.25) (0.75,2.5) (0.75,3) (1,3) (1.5,3) (2,3) "
                  "(2.25,3) (2.25,2.5) } { (1.625,1.5) (1.5,1.625) (1.375,1.5) "
                  "(1.5,1.375) (1.625,1.5) } } ");
    }
    {
        std::ostringstream ostr;
        w.out(ostr, 21.0);
        // "Polygon #2"
        EXPECT_EQ(ostr.str(), "{ { (1.625,1.5) (1.5,1.625) (1.375,1.5) "
                              "(1.5,1.375) (1.625,1.5) } } ");
    }
}

TEST_F(test_ms_polygon, three_nested_rings)
{
    // Three nested rings
    std::vector<double> data = {2, 2, 2, 2, 2, 2, 4, 4, 4, 2, 2, 4, 6,
                                4, 2, 2, 4, 4, 4, 2, 2, 2, 2, 2, 2};
    TestPolygonWriter w;
    {
        PolygonRingAppender<TestPolygonWriter> appender(w);
        IntervalLevelRangeIterator levels(
            1.0, 2.0, -std::numeric_limits<double>::infinity());
        SegmentMerger<PolygonRingAppender<TestPolygonWriter>,
                      IntervalLevelRangeIterator>
            writer(appender, levels, /* polygonize */ true);
        ContourGenerator<decltype(writer), IntervalLevelRangeIterator> cg(
            5, 5, false, NaN, writer, levels);
        for (int i = 0; i < 5; i++)
        {
            cg.feedLine(&data[5 * i]);
        }
    }
    {
        std::ostringstream ostr;
        w.out(ostr, 1.0);
        // "Polygon #0"
        EXPECT_EQ(ostr.str(), "");
    }
    {
        std::ostringstream ostr;
        w.out(ostr, 3.0);
        // "Polygon #1"
        EXPECT_EQ(
            ostr.str(),
            "{ { (4.5,5) (5,5) (5,4.5) (5,4) (5,3.5) (5,3) (5,2.5) (5,2) "
            "(5,1.5) (5,1) (5,0.5) (5,0) (4.5,0) (4,0) (3.5,0) (3,0) (2.5,0) "
            "(2,0) (1.5,0) (1,0) (0.5,0) (0,0) (0,0.5) (0,1) (0,1.5) (0,2) "
            "(0,2.5) (0,3) (0,3.5) (0,4) (0,4.5) (0,5) (0.5,5) (1,5) (1.5,5) "
            "(2,5) (2.5,5) (3,5) (3.5,5) (4,5) (4.5,5) } { (4,3.5) (3.5,4) "
            "(2.5,4) (1.5,4) (1,3.5) (1,2.5) (1,1.5) (1.5,1) (2.5,1) (3.5,1) "
            "(4,1.5) (4,2.5) (4,3.5) } } ");
    }
    {
        std::ostringstream ostr;
        w.out(ostr, 5.0);
        // "Polygon #2"
        EXPECT_EQ(ostr.str(),
                  "{ { (4,3.5) (3.5,4) (2.5,4) (1.5,4) (1,3.5) (1,2.5) (1,1.5) "
                  "(1.5,1) (2.5,1) (3.5,1) (4,1.5) (4,2.5) (4,3.5) } { (3,2.5) "
                  "(2.5,3) (2,2.5) (2.5,2) (3,2.5) } } ");
    }
    {
        std::ostringstream ostr;
        w.out(ostr, 7.0);
        // "Polygon #3"
        EXPECT_EQ(ostr.str(),
                  "{ { (3,2.5) (2.5,3) (2,2.5) (2.5,2) (3,2.5) } } ");
    }

    // "Inner ring of polygon #1 = exterioring ring of polygon #2"
    EXPECT_TRUE(
        equal_linestrings(w.polygons_[3.0][0][1], w.polygons_[5.0][0][0]));
    // "Inner ring of polygon #2 = exterioring ring of polygon #3"
    EXPECT_TRUE(
        equal_linestrings(w.polygons_[5.0][0][1], w.polygons_[7.0][0][0]));
}
}  // namespace
