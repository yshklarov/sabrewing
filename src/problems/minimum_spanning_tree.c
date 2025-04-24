/*
 * Problem: Minimum Spanning Tree (MSP)
 *
 * Input: A connected finite undirected graph, with nonnegative integer weights on the edges.
 *
 * Input parameter: n = |V| + |E| for a graph G = (V, E).
 *    (This is because graphs are often stored in adjacency list representation, with space
 *     usage Θ(|V| + |E|) (provided the indices fit in a fixed-width integer type).
 *
 * Output: A spanning tree with minimal total edge weight. Note that a spanning tree always has
 *     |V| - 1 edges.
 *
 * Storage format: Input is an adjacency list: an array of (|V| + 2|E|) u32 objects:
 *     {<degree of v_1>, {vertices adjacent to v_1}, <degree of v_2>, {vertices adj. to v_1}, ... }.
 *     Note that there's some redundancy: each edge contributes to two adjacency lists.
 *
 * Considerations: Also, n could be the number of vertices, with edge weight given implicitly
 * by a function.
 */


char const* problem_description()
{
    return "Find a minimum spanning tree in an undirected graph.";
}

char const* sampler_output_description()
{
    return "connected edge-weighted graph of size n = |V| + |E|";
}

// Bytes required to store input (will be allocated prior to calling sampler).
u64 input_size(u32 n)
{
    // Ask the profiler to allocate enough space for our adjacency list. We require
    // 4 × (|V| + 2|E|) <= 4 × (2|V| + 2|E|) = 4 × (2n) bytes.
    return sizeof(u32) * (2 * n);
}

// Bytes required to store output (will be allocated prior to calling target). If this returns 0,
// then the target will be assumed to operate in-place, and will be passed a null pointer as the
// output location.
u64 output_size(u32 n)
{
    // Ask the profiler to allocate enough space for our spanning tree. We require
    // 4 × (|V| - 1) <= 4n bytes.
    return sizeof(u32) * n;
}
