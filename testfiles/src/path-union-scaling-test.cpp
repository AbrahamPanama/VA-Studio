// SPDX-License-Identifier: GPL-2.0-or-later
// BUG-028 deterministic baseline capture and document preservation outcomes.
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <thread>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <random>
#include <limits>
#include <2geom/svg-path-writer.h>
#include <2geom/rect.h>
#include <2geom/sbasis-geometric.h>
#include <set>
#include "doc-per-case-test.h"
#include "document-undo.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "path/path-util.h"
#include "xml/repr.h"
using namespace Inkscape;
namespace Inkscape::detail {
void set_union_parallel_pairs_threshold_for_testing(std::size_t);
void set_union_candidate_budget_for_testing(std::size_t);
void set_union_fallback_exhaustive_for_testing(bool);
void set_union_parallel_throw_pair_for_testing(unsigned, unsigned);
}
namespace {
std::string dot(double x, double y) {
    constexpr double k = 0.5522847498307936;
    std::ostringstream s; s << std::setprecision(17);
    s << "M"<<x+1<<","<<y<<" C"<<x+1<<","<<y+k<<" "<<x+k<<","<<y+1<<" "<<x<<","<<y+1
      <<" C"<<x-k<<","<<y+1<<" "<<x-1<<","<<y+k<<" "<<x-1<<","<<y
      <<" C"<<x-1<<","<<y-k<<" "<<x-k<<","<<y-1<<" "<<x<<","<<y-1
      <<" C"<<x+k<<","<<y-1<<" "<<x+1<<","<<y-k<<" "<<x+1<<","<<y<<" Z";
    return s.str();
}
std::size_t rss() {
#ifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) return info.resident_size;
#endif
    return 0;
}
double signed_area(Geom::PathVector const &pathv)
{
    double total = 0;
    for (auto const &path : pathv) {
        Geom::Point centroid; double area = 0;
        Geom::centroid(path.toPwSb(), centroid, area);
        total += area;
    }
    return total;
}
std::string fixture(int n, int edge) {
    std::ostringstream s;
    s << "<svg xmlns='http://www.w3.org/2000/svg' width='250' height='250' viewBox='0 0 250 250'>"
      << "<defs><clipPath id='clip'><path d='M-10,-10 H30 V240 H-10Z'/></clipPath>"
      << "<mask id='mask'><rect width='250' height='250' fill='white'/></mask><clipPath id='other-clip'><path d='M0,0 H8 V8 H0Z'/></clipPath><mask id='other-mask'><rect width='250' height='250' fill='white'/></mask></defs>"
      << "<rect id='unrelated' x='220' y='220' width='3' height='4' fill='blue'/>"
      << "<g id='layer' transform='translate(3,4)'><rect id='before' width='1' height='1'/>";
    std::mt19937 rng(28031);
    int cols = n == 1000 ? 25 : n == 3000 ? 50 : 100;
    for (int i=0;i<n;++i) {
        if (edge==9 && i==1) s << "<g id='nested-layer' transform='translate(7,8) scale(1.2,0.8)'>";
        double x=10+1.6*(i%cols)+(int(rng()%40001)-20000)*0.000001;
        double y=10+1.6*(i/cols)+(int(rng()%40001)-20000)*0.000001;
        std::string d=dot(x,y);
        if (edge) {
            x=10+(i%3)*2.; y=10+(i/3)*5.;
            if (edge==1) x=10,y=10; // coincident
            if (edge==3) x=10+i*4.; // islands
            d=dot(x,y);
            if (edge==4 || edge==5 || edge==10) d="M0,0 H20 V20 H0Z M5,5 H15 V15 H5Z";
            if (edge==6) d=i%2 ? "M0,0 V20 H20 V0Z" : "M0,0 H20 V20 H0Z";
            if (edge==7) d="M0,0 L20,20 L0,20 L20,0Z";
            if (edge==11) d="M0,0 L0.000000001,0 L0,0.000000001Z"; // all quantize away
            if (edge==12) d=i%2 ? "M0,0 L20,0 L10,0Z" : "M0,0 L20,0 L20,20"; // open area + zero area
            if (edge==13) d="M0,0 L20,0 L10,0Z"; // all zero area
            if (edge==14 || edge==15 || edge==16 || edge==18 || edge==19) {
                // Same narrow overlapping rectangles, in very small/large local units
                // and at a large document translation (applied below).
                double scale=(edge==14 || edge==18) ? 1e-6 : (edge==15 || edge==19) ? 1e6 : 1.;
                std::ostringstream q; q<<std::setprecision(17);
                double left=i*0.2*scale, right=(10+i*0.2)*scale;
                q<<"M"<<left<<",0 H"<<right<<" V"<<0.03125*scale<<" H"<<left<<"Z";d=q.str();
            }
            if (edge==17) {
                // Disjoint donuts: each fill rule must retain its own hole.
                double left=i*30.; std::ostringstream q;
                q<<"M"<<left<<",0 h20 v20 h-20Z M"<<left+5<<",5 ";
                q<<(i%2 ? "h10 v10 h-10Z" : "v10 h10 v-10Z");d=q.str();
            }
            if (edge==20) d="M0,0 H20 V20 H0Z"; // dense coincident, no crossing growth
            if (edge==21) {
                double inset=double(i)/n*9.; std::ostringstream q;q<<std::setprecision(17);
                q<<"M"<<inset<<","<<inset<<" H"<<20-inset<<" V"<<20-inset<<" H"<<inset<<"Z";d=q.str();
            }
            if (edge==22) {
                // Eight disconnected four-cubic lobes per operand (32 cubics).
                d.clear();for(int k=0;k<8;++k) d+=dot(10+(i%cols)*30.+k*3.,10+(i/cols)*5.);
            }
            if (edge==8) d=i ? dot(10,10) : "M0,0 L0.000000001,0 L0,0.000000001Z";
        }
        s << "<path class='operand' id='p"<<i<<"' d='"<<d<<"' style='fill:"<<(i==0 ? "#2468ac" : "#cc7733")<<";fill-rule:"
          << ((edge==4 || ((edge==7 || edge==10 || edge==17) && i%2))?"evenodd":"nonzero")<<"'";
        std::string placement=edge==14 ? "scale(1000000)" : edge==15 ? "scale(0.000001)" : edge==16 ? "translate(100000000,-100000000)" : "translate(0,0)";
        if(i==0) s<<" clip-path='url(#clip)' mask='url(#mask)' transform='" << (edge==9 ? "translate(2,1) rotate(7)" : placement.c_str()) << "'";
        if(i>0) s<<" clip-path='url(#other-clip)' mask='url(#other-mask)'";
        if(i && edge>=14 && edge<=16) s<<" transform='"<<placement<<"'";
        if(edge==9 && i) s<<" transform='translate(4,3) rotate(15)'";
        s<<"/>";
    }
    if (edge==9 && n>1) s << "</g>";
    s<<"<rect id='after' x='230' width='1' height='1'/></g></svg>";return s.str();
}
std::string xml(SPObject *o) { return sp_repr_write_buf(o->getRepr(),0,false,Glib::QueryQuark(0u),0,0).raw(); }
}
class PathUnionScaling : public DocPerCaseTest {
protected:
    void capture(int n, int edge, char const *out = nullptr, bool reverse = false);
    double captured_seconds = 0;
};
void PathUnionScaling::capture(int n, int edge, char const *out, bool reverse) {
    auto input=fixture(n,edge);
    auto doc=SPDocument::createNewDocFromMem(input);ASSERT_TRUE(doc);doc->ensureUpToDate();
    auto objects=doc->getObjectsBySelector(".operand");ASSERT_EQ(objects.size(),n);
    auto a=doc->getObjectById("p0");
    std::string style=a->getAttribute("style"), transform=a->getAttribute("transform");
    auto unrelated=xml(doc->getObjectById("unrelated"));
    auto before=xml(doc->getObjectById("before")),after=xml(doc->getObjectById("after"));
    auto original=sp_repr_save_buf(doc->getReprDoc());
    DocumentUndo::done(doc.get(),Util::Internal::ContextString{"Fixture"},"");
    DocumentUndo::clearUndo(doc.get());DocumentUndo::clearRedo(doc.get());
    if(reverse) std::reverse(objects.begin(),objects.end());
    ObjectSet selection(doc.get());selection.setList(objects);
    auto initial_rss=rss();std::atomic<bool> sampling{true};std::size_t peak_rss=initial_rss;
    std::thread sampler([&] { while(sampling) { peak_rss=std::max(peak_rss,rss()); std::this_thread::sleep_for(std::chrono::milliseconds(2)); } });
    auto t=std::chrono::steady_clock::now();selection.pathUnion();
    double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count();
    captured_seconds=seconds;
    sampling=false;sampler.join();peak_rss=std::max(peak_rss,rss());
    std::cout<<"BUG028_RSS initial="<<initial_rss<<" peak="<<peak_rss<<" growth="<<peak_rss-initial_rss<<std::endl;
    if(auto limit=std::getenv("BUG028_MAX_GROWTH_MIB"); limit && (edge==20 || edge==21)) {
        EXPECT_GT(initial_rss,0) << "RSS sampling requires a supported platform";
        EXPECT_LE(peak_rss-initial_rss,std::strtoull(limit,nullptr,10)*1024*1024);
    }
    std::cout<<"BUG028 n="<<n<<" edge="<<edge<<" seconds="<<std::setprecision(17)<<seconds<<std::endl;
    auto result=selection.singleItem();
    bool empty=edge==11 || edge==13 || edge==18;
    std::string d;
    if(empty) {
        EXPECT_EQ(result,nullptr);EXPECT_EQ(doc->getObjectsBySelector(".operand").size(),0);
        for(int i=0;i<n;++i) EXPECT_EQ(doc->getObjectById(("p"+std::to_string(i)).c_str()),nullptr);
    } else {
        ASSERT_TRUE(result);
        EXPECT_STREQ(result->getId(),"p0");EXPECT_EQ(result->parent,doc->getObjectById("layer"));
        EXPECT_STREQ(result->getAttribute("style"),style.c_str());
        EXPECT_STREQ(result->getAttribute("transform"),transform.c_str());
        EXPECT_STREQ(result->getAttribute("clip-path"),"url(#clip)");
        EXPECT_STREQ(result->getAttribute("mask"),"url(#mask)");
        EXPECT_EQ(result->getRepr()->position(),1);
        EXPECT_EQ(xml(doc->getObjectById("unrelated")),unrelated);
        EXPECT_EQ(xml(doc->getObjectById("before")),before);EXPECT_EQ(xml(doc->getObjectById("after")),after);
        auto curve=curve_for_item(result);ASSERT_TRUE(curve);
        d=Geom::write_svg_path(*curve * result->i2doc_affine(),17);
        if(edge==17) for(int i=0;i<n;++i) {
            auto geometry=*curve * result->i2doc_affine();
            EXPECT_EQ(geometry.winding(Geom::Point(i*30.+13,14)),0); // hole center, layer +3,+4
            EXPECT_NE(geometry.winding(Geom::Point(i*30.+5,6)),0); // filled border
        }
    }
    EXPECT_EQ(xml(doc->getObjectById("unrelated")),unrelated);
    EXPECT_EQ(xml(doc->getObjectById("before")),before);EXPECT_EQ(xml(doc->getObjectById("after")),after);
    auto saved=sp_repr_save_buf(doc->getReprDoc());
    if(out) {
        std::ofstream(std::string(out)+".input.svg")<<input;
        std::ofstream(std::string(out)+".svg")<<saved;
        std::ofstream(std::string(out)+".d")<<d;
        std::ofstream(std::string(out)+".time")<<std::setprecision(17)<<seconds<<"\n";
    }
    auto reopened=SPDocument::createNewDocFromMem(saved.raw());ASSERT_TRUE(reopened);reopened->ensureUpToDate();
    if(empty) EXPECT_EQ(reopened->getObjectById("p0"),nullptr);
    else EXPECT_EQ(xml(reopened->getObjectById("p0")),xml(result));
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));doc->ensureUpToDate();
    EXPECT_EQ(doc->getObjectsBySelector(".operand").size(),n);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()),original);
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()),saved);EXPECT_FALSE(DocumentUndo::redo(doc.get()));
}

TEST_F(PathUnionScaling, CaptureAndPreserve) {
    int n=std::getenv("BUG028_N") ? std::atoi(std::getenv("BUG028_N")) : 9;
    int edge=std::getenv("BUG028_EDGE") ? std::atoi(std::getenv("BUG028_EDGE")) : 9;
    capture(n,edge,std::getenv("BUG028_OUTPUT"),std::getenv("BUG028_REVERSE") != nullptr);
}
TEST_F(PathUnionScaling, SingleComponentPathUnchanged) {
    // This exact document-space path string was captured at HEAD 6a7b239163 by
    // running this test before the disjoint-component product change.
    auto doc = SPDocument::createNewDocFromMem(fixture(4, 0));
    ASSERT_TRUE(doc); doc->ensureUpToDate();
    auto objects = doc->getObjectsBySelector(".operand");
    ASSERT_EQ(objects.size(), 4);
    ObjectSet selection(doc.get()); selection.setList(objects); selection.pathUnion();
    auto result = selection.singleItem(); ASSERT_TRUE(result);
    auto curve = curve_for_item(result); ASSERT_TRUE(curve);
    auto d = Geom::write_svg_path(*curve * result->i2doc_affine(), 17);
    EXPECT_EQ(d, "M 14.6171875 13.00390625 C 14.28699351 13.00390625 13.994605200000001 13.165372585 13.8125 13.412109375 C 13.63001983 13.169704411 13.338499840000001 13.01171875 13.01171875 13.01171875 C 12.459434 13.01171875 12.01171875 13.459434 12.01171875 14.01171875 S 12.459434 15.01171875 13.01171875 15.01171875 C 13.341912750000001 15.01171875 13.636254170000001 14.852205550000001 13.81835938 14.60546875 C 14.000839539999999 14.847873699999999 14.29040642 15.00390625 14.6171875 15.00390625 C 14.93829448 15.00390625 15.223284509999999 14.852439240000001 15.40625 14.6171875 C 15.58877305 14.85904721 15.87872784 15.015625 16.20507812 15.015625 C 16.530423380000002 15.015625 16.819333909999997 14.86168279 17.00195312 14.62109375 C 17.184684359999999 14.86010581 17.472779160000002 15.01367188 17.796875 15.01367188 C 18.349159749999998 15.01367188 18.796875 14.56595662 18.796875 14.01367188 C 18.796875 13.461387125 18.349159749999998 13.013671875 17.796875 13.013671875 C 17.471529750000002 13.013671875 17.182619219999999 13.169567218999999 17 13.41015625 C 16.817268759999997 13.171144186999999 16.529173970000002 13.015625 16.20507812 13.015625 C 15.883971150000001 13.015625 15.59898111 13.167092022 15.41601562 13.40234375 C 15.23349258 13.160484034 14.943537790000001 13.00390625 14.6171875 13.00390625 z");
}

std::string disjoint_component_fixture()
{
    auto svg = fixture(300, 22);
    auto const close = svg.rfind("</g></svg>");
    std::ostringstream extra;
    extra << "<path class='operand' id='cluster-a0' d='M2000,1000 H2020 V1020 H2000Z'/>"
          << "<path class='operand' id='cluster-a1' d='M2010,1000 H2030 V1020 H2010Z'/>"
          << "<path class='operand' id='cluster-a2' d='M2020,1000 H2040 V1020 H2020Z'/>"
          << "<path class='operand' id='cluster-b0' d='M2000,1100 H2020 V1120 H2000Z'/>"
          << "<path class='operand' id='cluster-b1' d='M2010,1100 H2030 V1120 H2010Z'/>"
          << "<path class='operand' id='cluster-b2' d='M2020,1100 H2040 V1120 H2020Z'/>"
          << "<path class='operand' id='donut' fill-rule='evenodd' d='M2100,1000 H2130 V1030 H2100Z M2110,1010 H2120 V1020 H2110Z'/>";
    svg.insert(close, extra.str());
    return svg;
}

TEST_F(PathUnionScaling, DisjointComponentsMatchIndependentOracle)
{
    auto input = disjoint_component_fixture();
    auto doc = SPDocument::createNewDocFromMem(input); ASSERT_TRUE(doc); doc->ensureUpToDate();
    auto operands = doc->getObjectsBySelector(".operand"); ASSERT_EQ(operands.size(), 307);
    double lobe_area = 0;
    for (unsigned i = 0; i < 300; ++i) {
        auto item = cast<SPItem>(operands[i]); ASSERT_TRUE(item);
        auto curve = curve_for_item(item); ASSERT_TRUE(curve);
        auto document_path = *curve * item->i2doc_affine();
        lobe_area += std::abs(signed_area(document_path));
    }
    auto original = sp_repr_save_buf(doc->getReprDoc());
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"Fixture"}, "");
    DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    ObjectSet selection(doc.get()); selection.setList(operands); selection.pathUnion();
    auto result = selection.singleItem(); ASSERT_TRUE(result);
    auto curve = curve_for_item(result); ASSERT_TRUE(curve);
    auto output = *curve * result->i2doc_affine();
    double output_area = signed_area(output);
    auto const expected_area = lobe_area + 800 + 800 + 800;
    EXPECT_NEAR(std::abs(output_area), expected_area, expected_area * 0.001);
    auto check = [&](Geom::Point p, bool inside) {
        auto winding = output.winding(p);
        EXPECT_EQ(winding != 0, inside);
        EXPECT_EQ(std::abs(winding) % 2 != 0, inside);
    };
    for (unsigned i = 0; i < 300; ++i) {
        for (unsigned k = 0; k < 8; ++k) {
            check(Geom::Point(13 + (i % 100) * 30 + k * 3, 14 + (i / 100) * 5), true);
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        check(Geom::Point(2008 + i * 10, 1014), true);
        check(Geom::Point(2008 + i * 10, 1114), true);
    }
    check(Geom::Point(2108, 1008), true);
    check(Geom::Point(2118, 1018), false); // evenodd donut hole
    check(Geom::Point(2070, 1008), false); // gap between components
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), original);
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
}

TEST_F(PathUnionScaling, DisjointComplexUnionIsLinear)
{
    capture(3000, 22);
    EXPECT_LT(captured_seconds, 10.0) << "BUG028 3000 disjoint 8-lobe union seconds=" << captured_seconds;
}
TEST_F(PathUnionScaling, AllOperandsQuantizeEmpty) {
    for(bool reverse : {false,true}) capture(9,11,nullptr,reverse);
}
TEST_F(PathUnionScaling, OpenAndZeroAreaOperands) {
    for(int edge : {12,13}) for(bool reverse : {false,true}) capture(9,edge,nullptr,reverse);
}
TEST_F(PathUnionScaling, SliversAtExtremeScalesAndTranslation) {
    for(int edge : {14,15,16,18,19}) for(bool reverse : {false,true}) capture(9,edge,nullptr,reverse);
}
TEST_F(PathUnionScaling, MixedFillRulesKeepBothSetsOfHoles) {
    for(bool reverse : {false,true}) capture(9,17,nullptr,reverse);
}

namespace Inkscape::detail {
// Test the production enumerator without adding a public geometry API/header.
bool union_candidates_bounded(std::vector<Geom::Rect> const &,
                              std::vector<std::pair<unsigned,unsigned>> &, std::size_t);
}

TEST_F(PathUnionScaling, SweepIncludesExactlyTouchingAndOverlappingBoxes) {
    std::vector<Geom::Rect> boxes;
    std::mt19937 rng(28031);
    for (int i=0;i<1000;++i) {
        double x=int(rng()%100),y=int(rng()%100);
        boxes.emplace_back(Geom::Point(x,y),Geom::Point(x+i%4,y+i%5));
    }
    for (int order=0;order<2;++order) {
        std::vector<std::pair<unsigned,unsigned>> swept;
        ASSERT_TRUE(detail::union_candidates_bounded(boxes,swept,8*1024*1024));
        std::set<std::pair<unsigned,unsigned>> actual,expected;
        EXPECT_TRUE(std::is_sorted(swept.begin(),swept.end()));
        for(auto [i,j] : swept) EXPECT_TRUE(actual.emplace(j,i).second);
        for(unsigned i=0;i<boxes.size();++i) for(unsigned j=0;j<i;++j) {
            if(boxes[i].intersects(boxes[j])) expected.emplace(j,i);
        }
        EXPECT_EQ(actual,expected);
        std::reverse(boxes.begin(),boxes.end());
    }
}

TEST_F(PathUnionScaling, CandidateBudgetFallsBackBeforeStoringAnotherPair) {
    using Pair=std::pair<unsigned,unsigned>;
    std::vector<Pair> pairs;
    std::vector<Geom::Rect> boxes(4,Geom::Rect(Geom::Point(0,0),Geom::Point(10,10)));
    ASSERT_TRUE(detail::union_candidates_bounded(boxes,pairs,6*sizeof(Pair)));
    EXPECT_EQ(pairs.size(),6);
    EXPECT_LE(pairs.capacity()*sizeof(Pair),6*sizeof(Pair));
    EXPECT_FALSE(detail::union_candidates_bounded(boxes,pairs,6*sizeof(Pair)-1));
    EXPECT_EQ(pairs.size(),0);EXPECT_EQ(pairs.capacity(),0); // partial list released
    EXPECT_FALSE(detail::union_candidates_bounded(boxes,pairs,0));
    EXPECT_EQ(pairs.capacity(),0);
    boxes={Geom::Rect(Geom::Point(0,0),Geom::Point(1,1)),
           Geom::Rect(Geom::Point(2,2),Geom::Point(3,3))};
    EXPECT_TRUE(detail::union_candidates_bounded(boxes,pairs,0));
    EXPECT_TRUE(pairs.empty());
}
TEST_F(PathUnionScaling, DenseCandidateStorageIsBoundedAtBothLargeSizes) {
    for(unsigned n : {3000,12000}) {
        std::vector<Geom::Rect> boxes(n,Geom::Rect(Geom::Point(0,0),Geom::Point(10,10)));
        std::vector<std::pair<unsigned,unsigned>> pairs;
        EXPECT_FALSE(detail::union_candidates_bounded(boxes,pairs,8*1024*1024));
        EXPECT_EQ(pairs.capacity(),0);
    }
}

TEST_F(PathUnionScaling, ParallelIntersectionsPreserveExactPathData)
{
    auto run = [](std::size_t threshold) {
        detail::set_union_parallel_pairs_threshold_for_testing(threshold);
        std::ostringstream svg;
        svg << "<svg xmlns='http://www.w3.org/2000/svg'><g>";
        std::vector<Geom::Rect> bounds;
        for (int i = 0; i < 40; ++i) {
            auto const x = (i % 8) * 0.5;
            auto const y = (i / 8) * 0.5;
            constexpr double size = 4.0;
            constexpr double radius = 0.5;
            constexpr double k = 0.5522847498307936;
            std::ostringstream d;
            d << std::setprecision(17) << "M" << x + radius << "," << y << " H" << x + size - radius
              << " C" << x + size - radius + k * radius << "," << y << " " << x + size << ","
              << y + radius - k * radius << " " << x + size << "," << y + radius << " V" << y + size - radius
              << " C" << x + size << "," << y + size - radius + k * radius << " " << x + size - radius + k * radius
              << "," << y + size << " " << x + size - radius << "," << y + size << " H" << x + radius
              << " C" << x + radius - k * radius << "," << y + size << " " << x << ","
              << y + size - radius + k * radius << " " << x << "," << y + size - radius << " V" << y + radius
              << " C" << x << "," << y + radius - k * radius << " " << x + radius - k * radius << "," << y
              << " " << x + radius << "," << y << " Z";
            svg << "<path class='operand' d='" << d.str() << "'/>";
            bounds.emplace_back(Geom::Point(x, y), Geom::Point(x + size, y + size));
        }
        svg << "</g></svg>";
        std::vector<std::pair<unsigned, unsigned>> candidates;
        EXPECT_TRUE(detail::union_candidates_bounded(bounds, candidates, 8 * 1024 * 1024));
        EXPECT_GT(candidates.size(), 256);
        auto doc = SPDocument::createNewDocFromMem(svg.str());
        EXPECT_TRUE(doc);
        if (!doc) return std::string{};
        doc->ensureUpToDate();
        auto operands = doc->getObjectsBySelector(".operand");
        EXPECT_EQ(operands.size(), 40);
        ObjectSet selection(doc.get());
        selection.setList(operands);
        selection.pathUnion();
        auto result = selection.singleItem();
        EXPECT_TRUE(result);
        auto curve = result ? curve_for_item(result) : std::nullopt;
        EXPECT_TRUE(curve);
        return curve ? Geom::write_svg_path(*curve, 17) : std::string{};
    };
    auto const serial = run(std::numeric_limits<std::size_t>::max());
    auto const parallel = run(0);
    detail::set_union_parallel_pairs_threshold_for_testing(256);
    EXPECT_FALSE(serial.empty());
    EXPECT_EQ(parallel, serial);
}

TEST_F(PathUnionScaling, ExhaustiveParallelIntersectionsPreserveExactPathData)
{
    detail::set_union_candidate_budget_for_testing(0);
    auto run = [](std::size_t threshold) {
        detail::set_union_parallel_pairs_threshold_for_testing(threshold);
        std::ostringstream svg;
        svg << "<svg xmlns='http://www.w3.org/2000/svg'><g>";
        for (int i = 0; i < 300; ++i) {
            auto const x = i == 1 ? 0.5 : i * 4.0;
            svg << "<path class='operand' d='M" << x << ",0 h2 v2 h-2 Z'/>";
        }
        svg << "</g></svg>";
        auto doc = SPDocument::createNewDocFromMem(svg.str());
        EXPECT_TRUE(doc);
        if (!doc) return std::string{};
        doc->ensureUpToDate();
        auto operands = doc->getObjectsBySelector(".operand");
        EXPECT_EQ(operands.size(), 300);
        ObjectSet selection(doc.get());
        selection.setList(operands);
        selection.pathUnion();
        auto result = selection.singleItem();
        auto curve = result ? curve_for_item(result) : std::nullopt;
        EXPECT_TRUE(curve);
        return curve ? Geom::write_svg_path(*curve, 17) : std::string{};
    };
    auto const serial = run(std::numeric_limits<std::size_t>::max());
    auto const parallel = run(0);
    detail::set_union_parallel_pairs_threshold_for_testing(256);
    detail::set_union_candidate_budget_for_testing(8 * 1024 * 1024);
    EXPECT_FALSE(serial.empty());
    EXPECT_EQ(parallel, serial);
}

TEST_F(PathUnionScaling, StreamedFallbackMatchesExhaustiveWithMixedBounds)
{
    auto make_result = [](bool exhaustive, std::size_t threshold) {
        detail::set_union_candidate_budget_for_testing(1);
        detail::set_union_fallback_exhaustive_for_testing(exhaustive);
        detail::set_union_parallel_pairs_threshold_for_testing(threshold);
        std::ostringstream svg;
        svg << "<svg xmlns='http://www.w3.org/2000/svg'><g>";
        for (int i = 0; i < 80; ++i) {
            auto const radius = 12.0 + i;
            svg << "<path class='operand' fill-rule='evenodd' d='M" << 50-radius << "," << 50-radius
                << " H" << 50+radius << " V" << 50+radius << " H" << 50-radius << " Z M"
                << 50-radius/2 << "," << 50-radius/2 << " H" << 50+radius/2 << " V"
                << 50+radius/2 << " H" << 50-radius/2 << " Z'/>";
        }
        for (int i = 0; i < 80; ++i) {
            auto const half = 8.0 + i * 0.2;
            svg << "<path class='operand' d='M" << 100-half << "," << 100-half << " H"
                << 100+half << " V" << 100+half << " H" << 100-half << " Z'/>";
        }
        for (int i = 0; i < 40; ++i) {
            auto const x = 200.0 + i * 30;
            svg << "<path class='operand' d='M" << x << ",0 h10 v10 h-10 Z'/>"
                << "<path class='operand' d='M" << x+10 << ",0 h10 v10 h-10 Z'/>";
        }
        for (int i = 0; i < 40; ++i) {
            auto const x = 300.0 + i * 3;
            svg << "<path class='operand' d='M" << x << ",200 V220'/>";
        }
        for (int i = 0; i < 40; ++i) {
            auto const y = 200.0 + i * 3;
            svg << "<path class='operand' d='M400," << y << " H420'/>";
        }
        for (int i = 0; i < 40; ++i) {
            auto const x = 600.0 + i * 24;
            auto const y = 200.0 + (i % 5) * 24;
            svg << "<path class='operand' d='M" << x << "," << y+4 << " h16 v8 h-16 Z'/>"
                << "<path class='operand' d='M" << x+4 << "," << y << " h8 v16 h-8 Z'/>";
        }
        svg << "</g></svg>";
        auto doc = SPDocument::createNewDocFromMem(svg.str());
        EXPECT_TRUE(doc);
        if (!doc) return std::string{};
        doc->ensureUpToDate();
        auto operands = doc->getObjectsBySelector(".operand");
        EXPECT_EQ(operands.size(), 400);
        ObjectSet selection(doc.get());
        selection.setList(operands);
        selection.pathUnion();
        auto result = selection.singleItem();
        EXPECT_TRUE(result);
        auto curve = result ? curve_for_item(result) : std::nullopt;
        EXPECT_TRUE(curve);
        return curve ? Geom::write_svg_path(*curve, 17) : std::string{};
    };
    auto const serial_streamed = make_result(false, std::numeric_limits<std::size_t>::max());
    auto const serial_exhaustive = make_result(true, std::numeric_limits<std::size_t>::max());
    auto const parallel_streamed = make_result(false, 0);
    auto const parallel_exhaustive = make_result(true, 0);
    detail::set_union_fallback_exhaustive_for_testing(false);
    detail::set_union_parallel_pairs_threshold_for_testing(256);
    detail::set_union_candidate_budget_for_testing(8 * 1024 * 1024);
    ASSERT_FALSE(serial_streamed.empty());
    EXPECT_EQ(serial_streamed, serial_exhaustive);
    EXPECT_EQ(parallel_streamed, parallel_exhaustive);
    EXPECT_EQ(parallel_streamed, serial_streamed);
}

TEST_F(PathUnionScaling, ParallelIntersectionExceptionReturnsToCaller)
{
    detail::set_union_candidate_budget_for_testing(0);
    detail::set_union_parallel_pairs_threshold_for_testing(0);
    detail::set_union_parallel_throw_pair_for_testing(1, 0);
    auto doc = SPDocument::createNewDocFromMem(
        "<svg xmlns='http://www.w3.org/2000/svg'><path class='operand' d='M0,0 h10 v10 h-10 Z'/>"
        "<path class='operand' d='M5,0 h10 v10 h-10 Z'/></svg>");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    ObjectSet selection(doc.get());
    selection.setList(doc->getObjectsBySelector(".operand"));
    EXPECT_THROW(selection.pathUnion(), std::runtime_error);
    detail::set_union_parallel_throw_pair_for_testing(std::numeric_limits<unsigned>::max(),
                                                       std::numeric_limits<unsigned>::max());
    detail::set_union_parallel_pairs_threshold_for_testing(256);
    detail::set_union_candidate_budget_for_testing(8 * 1024 * 1024);
}
