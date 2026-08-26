#include "ReactiveGraph.hpp"

#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace nvidia::write_protect
{

class ReactiveGraphTest : public ::testing::Test
{
  protected:
    Graph graph;

    std::vector<bool> observed;

    void observe(NodeId id)
    {
        graph.subscribe(id, [this](bool v) { observed.push_back(v); });
    }
};

TEST_F(ReactiveGraphTest, SourceDefaultsToFalse)
{
    auto src = graph.addSource();
    EXPECT_FALSE(graph.output(src));
}

TEST_F(ReactiveGraphTest, ComputeNodeWithNoInputsIsFalse)
{
    EXPECT_FALSE(graph.output(graph.addNode(BoolOp::Or)));
    EXPECT_FALSE(graph.output(graph.addNode(BoolOp::And)));
}

TEST_F(ReactiveGraphTest, OrNodeFollowsAnyInput)
{
    auto a = graph.addSource();
    auto b = graph.addSource();
    auto node = graph.addNode(BoolOp::Or);
    graph.connect(a, node);
    graph.connect(b, node);

    graph.set(a, true);
    graph.propagate();
    EXPECT_TRUE(graph.output(node));

    graph.set(a, false);
    graph.propagate();
    EXPECT_FALSE(graph.output(node));
}

TEST_F(ReactiveGraphTest, AndNodeRequiresAllInputs)
{
    auto a = graph.addSource();
    auto b = graph.addSource();
    auto node = graph.addNode(BoolOp::And);
    graph.connect(a, node);
    graph.connect(b, node);

    graph.set(a, true);
    graph.propagate();
    EXPECT_FALSE(graph.output(node));

    graph.set(b, true);
    graph.propagate();
    EXPECT_TRUE(graph.output(node));
}

TEST_F(ReactiveGraphTest, ObserverFiresOnlyOnChange)
{
    auto src = graph.addSource();
    auto node = graph.addNode(BoolOp::Or);
    graph.connect(src, node);
    observe(node);

    graph.set(src, true);
    graph.propagate();
    ASSERT_EQ(observed, std::vector<bool>{true});

    // Same value again: no notification.
    graph.set(src, true);
    graph.propagate();
    ASSERT_EQ(observed, std::vector<bool>{true});

    graph.set(src, false);
    graph.propagate();
    ASSERT_EQ(observed, (std::vector<bool>{true, false}));
}

// Regression: a node linked to an already-true source must reflect the
// source's value immediately, and its observers must be notified on the
// next propagate.  Previously connect() only marked the target dirty and
// propagate() never re-evaluated it, so the target stayed false until the
// source's next transition.
TEST_F(ReactiveGraphTest, ConnectToAlreadyTrueSourceSeedsTarget)
{
    auto src = graph.addSource();
    graph.set(src, true);
    graph.propagate();

    auto node = graph.addNode(BoolOp::Or);
    graph.connect(src, node);
    EXPECT_TRUE(graph.output(node));

    observe(node);
    graph.propagate();
    ASSERT_EQ(observed, std::vector<bool>{true});
}

TEST_F(ReactiveGraphTest, ConnectToAlreadyTrueNodeSeedsDownstream)
{
    auto src = graph.addSource();
    auto upstream = graph.addNode(BoolOp::Or);
    graph.connect(src, upstream);
    graph.set(src, true);
    graph.propagate();

    auto downstream = graph.addNode(BoolOp::Or);
    graph.connect(upstream, downstream);
    EXPECT_TRUE(graph.output(downstream));
}

// And nodes must behave the same: their initial value carries no And
// identity, so seeding from already-true sources still notifies.
TEST_F(ReactiveGraphTest, ConnectToAlreadyTrueSourcesSeedsAndTarget)
{
    auto a = graph.addSource();
    auto b = graph.addSource();
    graph.set(a, true);
    graph.set(b, true);
    graph.propagate();

    auto node = graph.addNode(BoolOp::And);
    graph.connect(a, node);
    graph.connect(b, node);
    EXPECT_TRUE(graph.output(node));

    observe(node);
    graph.propagate();
    ASSERT_EQ(observed, std::vector<bool>{true});
}

TEST_F(ReactiveGraphTest, NoInputAndNodeReadsFalseDownstream)
{
    auto empty = graph.addNode(BoolOp::And);
    auto node = graph.addNode(BoolOp::Or);
    graph.connect(empty, node);
    EXPECT_FALSE(graph.output(node));
}

TEST_F(ReactiveGraphTest, SharedSourceDrivesMultipleGroups)
{
    auto src = graph.addSource();
    auto g1 = graph.addNode(BoolOp::Or);
    auto g2 = graph.addNode(BoolOp::Or);
    graph.connect(src, g1);
    graph.connect(src, g2);
    observe(g1);
    observe(g2);

    graph.set(src, true);
    graph.propagate();
    EXPECT_TRUE(graph.output(g1));
    EXPECT_TRUE(graph.output(g2));
    EXPECT_EQ(observed.size(), 2U);
}

// An adopted placeholder source node (a WriteProtectInput whose name was
// first referenced by a group) is left untouched by link resolution: it
// keeps its edges and observers, and later value changes flow through.
TEST_F(ReactiveGraphTest, AdoptedSourceKeepsEdgesAndObservers)
{
    auto placeholder = graph.addSource();
    auto node = graph.addNode(BoolOp::Or);
    graph.connect(placeholder, node);

    // Observers on the source node itself, as addSoftwareToGroup installs
    // for an input's own FlashProtectedComponents.
    observe(placeholder);
    observe(node);

    graph.set(placeholder, true);
    graph.propagate();
    EXPECT_TRUE(graph.output(node));
    EXPECT_EQ(observed, (std::vector<bool>{true, true}));
}

// Documents why resolveGroupLinks must not deactivate an adopted node:
// deactivate() severs all edges (duplicates included), clears observers,
// and resets the node, leaving downstream nodes with no inputs.
TEST_F(ReactiveGraphTest, DeactivateResetsNodeAndReevaluatesChildren)
{
    auto src = graph.addSource();
    auto node = graph.addNode(BoolOp::Or);
    graph.connect(src, node);
    // A second, duplicate edge does not survive deactivation either.
    graph.connect(src, node);

    graph.set(src, true);
    graph.propagate();
    EXPECT_TRUE(graph.output(node));

    observe(src);
    graph.deactivate(src);
    EXPECT_FALSE(graph.output(node));

    // The deactivated node's observers are gone.
    graph.set(src, true);
    graph.propagate();
    EXPECT_TRUE(observed.empty());
}

TEST_F(ReactiveGraphTest, DeactivateNotifiesDownstreamObservers)
{
    auto src = graph.addSource();
    auto node = graph.addNode(BoolOp::Or);
    graph.connect(src, node);
    graph.set(src, true);
    graph.propagate();

    observe(node);
    graph.deactivate(src);
    graph.propagate();
    ASSERT_EQ(observed, std::vector<bool>{false});
}

TEST_F(ReactiveGraphTest, ConnectIntoSourceThrows)
{
    auto src = graph.addSource();
    auto node = graph.addNode(BoolOp::Or);
    EXPECT_THROW(graph.connect(node, src), std::runtime_error);
}

TEST_F(ReactiveGraphTest, ConnectCycleThrows)
{
    auto a = graph.addNode(BoolOp::Or);
    auto b = graph.addNode(BoolOp::Or);
    graph.connect(a, b);
    EXPECT_THROW(graph.connect(b, a), std::runtime_error);
    EXPECT_THROW(graph.connect(a, a), std::runtime_error);
}

} // namespace nvidia::write_protect
