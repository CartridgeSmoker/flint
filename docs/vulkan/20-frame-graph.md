# Chapter 20: Frame Graphs

> **Objectives:** Explain why passes form a directed acyclic graph; define nodes in JSON with attachment, texture, buffer, and reference resources; build producer edges from input names; order passes with topological sort; drive rendering with automatic layout transitions; reuse transient memory with reference-counted aliasing. Chapter 18 covers the single-pass subpass alternative, and Chapter 21 overlaps graph passes with async compute.

> **Flint note:** Flint fits in one graph: a 3D viewport pass writes a colour target, a UI pass samples it and composites widgets on top, and a final pass presents the result. One graph gives Flint ordering, transitions, and aliasing instead of three hand-wired render passes.

## 20.1 Passes want to be a graph

A single-pass renderer is easy: one render pass, one framebuffer, one set of barriers. It stops scaling once depth prepass, g-buffer, lighting, and post effects each need their own targets. Hand-ordering ten passes is tedious; hand-ordering a hundred is where bugs live.

A frame graph treats each pass as a node and derives order from data flow. If pass B reads a resource pass A writes, the graph holds a directed edge from A to B. The frame is then a directed acyclic graph (DAG): directed because edges point producer to consumer, acyclic because a cycle would ask a pass to run before itself.

The payoff is threefold. Passes and framebuffers are created from the declarations. Layouts and barriers are inferred where producer usage meets consumer usage. Transient resources with disjoint lifetimes share backing memory.

## 20.2 Declaring nodes as data

Some engines build graphs in code; this guide declares them in JSON parsed at startup. Topology stays in one readable file, and edits need no recompile. Each node has a name, inputs, and outputs:

```json
{
    "name": "gbuffer_pass",
    "inputs": [
        { "type": "attachment", "name": "depth" }
    ],
    "outputs": [
        { "type": "attachment", "name": "gbuffer_colour",
          "format": "VK_FORMAT_B8G8R8A8_UNORM", "resolution": [1280, 800],
          "op": "VK_ATTACHMENT_LOAD_OP_CLEAR" },
        { "type": "attachment", "name": "gbuffer_normals",
          "format": "VK_FORMAT_R16G16B16A16_SFLOAT", "resolution": [1280, 800],
          "op": "VK_ATTACHMENT_LOAD_OP_CLEAR" }
    ]
}
```

Four resource kinds cover the uses:

- **attachment:** bound to the render pass and framebuffer. It may appear as input and output when a pass continues work on a target, for example loading depth from a prepass.
- **texture:** sampled in shaders, such as a shadow map in lighting. Format and size are declared once at the producer; consumers name it only.
- **buffer:** storage shared across passes, with barrier needs like textures.
- **reference:** an ordering-only edge. A transparent pass may read `lighting` and pass it through unchanged; the next consumer must still run after it, but a second `lighting` attachment output would wrongly allocate a new target:

```json
{
    "name": "transparent_pass",
    "inputs": [
        { "type": "attachment", "name": "lighting" },
        { "type": "attachment", "name": "depth" }
    ],
    "outputs": [
        { "type": "reference", "name": "lighting" }
    ]
}
```

Every input must name a producer output, except externals such as the swapchain image, which the application supplies at runtime.

## 20.3 Resources, nodes, and the build sequence

Inputs and outputs are separate objects even when they name one image, because usage differs: one pass writes as an attachment, the next reads as a texture, and that difference selects the barrier.

```cpp
struct FrameGraphResource {
    FrameGraphResourceType type;
    FrameGraphResourceInfo info; // size, format, load op, external flag
    uint32_t producerNode = UINT32_MAX;
    uint32_t parentOutput = UINT32_MAX; // matching output, for inputs
    int32_t refCount = 0;               // live consumers, for aliasing
    const char* name = nullptr;
};

struct FrameGraphNode {
    const char* name = nullptr;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    FrameGraphRenderPass* implementation = nullptr; // virtual render() hook
    std::vector<uint32_t> inputs;
    std::vector<uint32_t> outputs;
    std::vector<uint32_t> edges; // consumers of this node
};
```

Startup is parse, compile, register, render:

```cpp
FrameGraphBuilder builder;
builder.init(&gpu);
FrameGraph graph;
graph.init(&builder);
graph.parse(graphPath);   // fill tables from JSON
graph.compile();          // edges, order, targets, aliasing
builder.registerRenderPass("gbuffer_pass", &gbufferPass);
builder.registerRenderPass("lighting_pass", &lightingPass);
graph.render(commandBuffer, scene);
```

Parsing fills tables; `compile` does all analysis. Implementations register by name, so swapping a technique means registering a different object under the same node name.

## 20.4 From names to edges

Compilation links each input to its producer by hashed name, copies format and size into the input, and records an edge from producer to consumer:

```cpp
for (uint32_t n = 0; n < nodes.size(); ++n) {
    for (uint32_t handle : nodes[n].inputs) {
        FrameGraphResource& input = resources[handle];
        uint32_t outHandle = findOutputByName(input.name);
        FrameGraphResource& output = resources[outHandle];
        input.info = output.info;
        input.producerNode = output.producerNode;
        input.parentOutput = outHandle;
        nodes[output.producerNode].edges.push_back(n);
    }
}
```

Each node now knows its consumers. Nodes nobody reads and with no side effects could be culled here; the guide's code does not yet do this.

## 20.5 Ordering with topological sort

Edges constrain but do not order. Topological sort linearises the DAG so producers run first, using depth-first search with three marks: unvisited, visiting, and done. Finishing a node appends it; reversing the list gives execution order:

```cpp
enum class Mark : uint8_t { Unvisited, Visiting, Done };

std::vector<uint32_t> order;
order.reserve(nodes.size());
std::vector<Mark> mark(nodes.size(), Mark::Unvisited);
std::vector<uint32_t> stack;

for (uint32_t n = 0; n < nodes.size(); ++n) {
    if (mark[n] != Mark::Unvisited)
        continue;
    stack.push_back(n);
    while (!stack.empty()) {
        uint32_t current = stack.back();
        if (mark[current] == Mark::Done) {
            stack.pop_back();
            continue;
        }
        if (mark[current] == Mark::Visiting) {
            mark[current] = Mark::Done;
            order.push_back(current);
            stack.pop_back();
            continue;
        }
        mark[current] = Mark::Visiting;
        for (uint32_t child : nodes[current].edges) {
            if (mark[child] == Mark::Unvisited)
                stack.push_back(child);
            // NOTE: mark[child] == Visiting indicates a cycle.
            // This listing ignores it - add a report here (see Exercise 1).
        }
    }
}
std::reverse(order.begin(), order.end());
```

The `Done` check keeps a producer feeding many consumers in the list once. A `Visiting` encounter indicates a cycle; the listing ignores it - add a report here (see Exercise 1).

## 20.6 Driving rendering and placing barriers

Execution walks the sorted order. Each node transitions texture inputs to shader-read and attachment outputs to attachment-optimal (depth via depth aspect), sets viewport and scissor from its target, then delegates to its implementation:

```cpp
for (uint32_t n : order) {
    FrameGraphNode& node = nodes[n];
    for (uint32_t handle : node.inputs)
        if (resources[handle].type == FrameGraphResourceType::Texture)
            transitionToReadable(cmd, resources[handle]);
    for (uint32_t handle : node.outputs)
        if (resources[handle].type == FrameGraphResourceType::Attachment)
            transitionToWritable(cmd, resources[handle]);

    setViewportAndScissor(cmd, targetExtent(node));
    node.implementation->preRender(cmd, scene); // work outside the pass
    vkCmdBeginRenderPass(cmd, &beginInfoFor(node), VK_SUBPASS_CONTENTS_INLINE);
    node.implementation->render(cmd, scene);
    vkCmdEndRenderPass(cmd);
}
```

`preRender` handles work outside the pass such as mipmap generation; `render` records draws. Per-node viewport setup keeps mixed-resolution chains correct without caller bookkeeping.

Barriers are mechanical once usage is known. A texture input written as an attachment moves to shader-read layout; a fresh output moves from undefined to attachment:

```cpp
VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
barrier.image = image;
barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
    0, 0, nullptr, 0, nullptr, 1, &barrier);
```

Depth uses the depth aspect; buffers use `VkBufferMemoryBarrier` from write to read access.

> **Misconception: "Sorted order makes barriers unnecessary."**
> Order alone is not visibility. Without a barrier, a fragment read may sample stale tiles even though the producing pass ran first. Order fixes when things run; barriers make writes visible to later stages. The graph earns its keep by inserting the second automatically wherever producer and consumer usage differ.

## 20.7 Reusing memory with aliasing

Transient targets dominate frame memory: g-buffer channels dead after lighting, bloom mips dead after composite. The graph aliases disjoint lifetimes onto shared store by counting consumers, then sweeping in order.

```cpp
for (uint32_t n : order)
    for (uint32_t handle : nodes[n].inputs)
        ++resources[resources[handle].parentOutput].refCount;

std::vector<VkDeviceMemory> freeList;
for (uint32_t n : order) {
    for (uint32_t handle : nodes[n].outputs) {
        FrameGraphResource& out = resources[handle];
        if (out.info.external)
            continue; // swapchain and history are never aliased
        // NOTE: the guide's code reuses only attachment outputs here;
        // this listing generalises to any non-reference output.
        if (!freeList.empty() && out.type != FrameGraphResourceType::Reference) {
            bindToAllocation(out, freeList.back());
            freeList.pop_back();
        } else {
            allocateBacking(out);
        }
    }
    for (uint32_t handle : nodes[n].inputs) {
        FrameGraphResource& out = resources[resources[handle].parentOutput];
        if (out.info.external)
            continue;
        if (--out.refCount == 0 && isImageLike(out))
            freeList.push_back(backingOf(out));
    }
}
```

First-fit reuse is greedy and can fragment; the listing simply reuses the most recently freed block. Recording allocating and freeing node per resource turns aliasing bugs into a printable lifetime table. Where Chapter 18 keeps intermediates on tile inside one pass, this graph reuses them across passes; Chapter 21 schedules its independent branches on async queues.

## Recap

- A frame graph is a DAG of passes: nodes declare inputs and outputs, edges run producer to consumer, cycles are errors.
- JSON keeps topology editable; attachment, texture, buffer, and reference kinds separate framebuffer membership, shader reads, storage, and ordering-only links.
- `compile` links names to edges, sorts topologically, creates passes and framebuffers, and plans aliasing.
- Depth-first search with unvisited, visiting, and done marks gives execution order; a visiting encounter indicates a cycle (reporting it is Exercise 1).
- The render walk transitions texture inputs to shader-read and attachment outputs to attachment-optimal, refreshes viewport and scissor per target, then calls `preRender` and `render`.
- Reference-counted lifetimes return dead transients to a free list; externals never alias.

## Exercises

1. **Cycle detector.** Add three nodes where A feeds B, B feeds C, and C feeds A. Add a check for `mark[child] == Mark::Visiting` that logs the cycle path and returns false/aborts `compile`, confirm it names the offender, then break the loop and show the order places every producer before its consumers.
2. **Lifetime table.** Log allocating node, last-consuming node, and reuse-or-allocate per transient output. Build a graph where a g-buffer target dies after lighting and a post target is born later, verify they share backing store, then mark the g-buffer external and confirm it no longer aliases.
