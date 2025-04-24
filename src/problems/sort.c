/**** Problem metadata ****/

char const* problem_description()
{
    return "Sort an array of 32-bit integers.";
}

char const* sampler_output_description()
{
    return "An array of length n.";
}

// Bytes required to store input (will be allocated prior to calling sampler).
u64 input_size(u32 n)
{
    return sizeof(u32) * n;
}

// Bytes required to store output (will be allocated prior to calling target). If this returns 0,
// then the target will be assumed to operate in-place, and will be passed a null pointer as the
// output location.
u64 output_size(u32 n)
{
    (void)n;
    return 0;
}

/**** Types ****/

typedef void (*fn_sampler)(u32* data, u32 n, RandState* rs, void* scratch);
typedef void (*fn_target)(u32* data, u32 n, RandState* rs, void* scratch);
typedef bool (*fn_verifier)(u32* input, u32* output, u32 n, RandState* rs, void* scratch);
typedef u64 (*fn_size)(u32 n);

typedef struct
{
    const char* name;
    const char* description;
    const fn_sampler fn;
    const fn_size scratch_size;
} Sampler;

typedef struct
{
    const char* name;
    const char* description;
    const fn_target fn;
    const fn_size scratch_size;
} Target;

typedef struct
{
    const char* name;
    const char* description;
    const fn_verifier fn;
    const fn_size scratch_size;
} Verifier;


/**** Forward declarations and function arrays ****/

void sample_uniform(u32*, u32, RandState*, void*);
void sample_ordered(u32*, u32, RandState*, void*);
void sample_almostordered(u32*, u32, RandState*, void*);
void sample_reversed(u32*, u32, RandState*, void*);
void sample_constant(u32*, u32, RandState*, void*);
void sample_mixture(u32*, u32, RandState*, void*);
static const Sampler samplers[] =
{
    {"Uniform", "Every array occurs with equal probability.", sample_uniform, NULL},
    {"Ordered", "The array is already sorted.", sample_ordered, NULL},
    {"Almost ordered", "Some random transpositions are applied.", sample_almostordered, NULL},
    {"Reversed", "The array is in reverse order.", sample_reversed, NULL},
    {"Constant", "All elements of the array are the same.", sample_constant, NULL},
    {"Mixture", "Pick a sampler at random each time.", sample_mixture, NULL},
};

void sort_heap(u32*, u32, RandState*, void*);
void sort_merge(u32*, u32, RandState*, void*);
u64 sort_merge_scratch_size(u32);
void sort_shell(u32*, u32, RandState*, void*);
void sort_quick(u32*, u32, RandState*, void*);
void sort_quickr(u32*, u32, RandState*, void*);
void sort_intro(u32*, u32, RandState*, void*);
void sort_insertion(u32*, u32, RandState*, void*);
void sort_selection(u32*, u32, RandState*, void*);
void sort_bubble(u32*, u32, RandState*, void*);
void sort_gnome(u32*, u32, RandState*, void*);
void sort_simple(u32*, u32, RandState*, void*);
void sort_broken(u32*, u32, RandState*, void*);
void sort_miracle(u32*, u32, RandState*, void*);
static const Target targets[] =
{
    {"Heapsort", "Builds max-heap, then moves root to end repeatedly.", sort_heap, NULL},
    {"Merge sort", "Sorts each half separately, then merges them.", sort_merge, sort_merge_scratch_size},
    {"Shellsort", "Insertion-sorts kth items for successively smaller k.", sort_shell, NULL},
    {"Quicksort", "Splits array according to a pivot, then sorts each side.", sort_quick, NULL},
    {"Quicksort (randomized)", "Picks the pivot randomly.", sort_quickr, NULL},
    {"Introsort", "Like quicksort, delegating to heap- and insertion sort.", sort_intro, NULL},
    {"Insertion sort", "Builds a sorted array element-by-element.", sort_insertion, NULL},
    {"Selection sort", "Finds least element of those remaining, and appends it.", sort_selection, NULL},
    {"Bubble sort", "Compares and swaps adjacent pairs.", sort_bubble, NULL},
    {"Gnome sort", "Holds one element, walking left or right.", sort_gnome, NULL},
    {"Simple sort", "Runs in a double loop, comparing and swapping.", sort_simple, NULL},
    {"Broken sort", "Heapsort, but deliberately fails occasionally.", sort_broken, NULL},
    // Disabled for now, because there's no way to kill the unresponsive worker thread.
    //{"Miracle sort", "Busy-waits for the list to be sorted.", sort_miracle, NULL},
};

bool verify_checksum(u32*, u32*, u32, RandState*, void*);
bool verify_ordered(u32*, u32*, u32, RandState*, void*);
bool verify_all(u32*, u32*, u32, RandState*, void*);
static const Verifier verifiers[] =
{
    {"All", "Runs all verifiers in sequence.", verify_all, NULL},
    {"Checksum", "Uses a commutative hash (invariant under permutations).", verify_checksum, NULL},
    {"Ordered", "Checks that the output is in ascending order.", verify_ordered, NULL},
};


/**** Samplers ****/

void sample_uniform(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch;
    for (u32 k = 0; k < n; ++k) {
        data[k] = rand_u32(rs);
    }
}

void sample_ordered(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch;
    if (n == 0) return;
    u32 value = 0;
    u32 max_stride = U32_MAX / n;
    for (u32 k = 0; k < n; ++k) {
        value += rand_range_unif(rs, 0, max_stride);
        data[k] = value;
    }
}

void sample_almostordered(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch;
    if (n == 0) return;
    sample_ordered(data, n, rs, scratch);
    u32 swap_count = 5;
    for (u32 k = 0; k < swap_count; ++k) {
        u32 i = rand_range_unif(rs, 0, n-1);
        u32 j = rand_range_unif(rs, 0, n-1);
        SWAP_u32(data[i], data[j]);
    }
}

void sample_reversed(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch;
    sample_ordered(data, n, rs, scratch);
    reverse_u32(n, data);
}

void sample_constant(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch;
    u32 value = rand_range_unif(rs, 0, n);
    for (u32 k = 0; k < n; ++k) {
        data[k] = value;
    }
}

void sample_mixture(u32* data, u32 n, RandState* rs, void* scratch)
{
    u32 choice = rand_range_unif(rs, 0, ARRAY_SIZE(samplers) - 2);
    samplers[choice].fn(data, n, rs, scratch);
}


/**** Targets ****/

// A silly variant of the exchange sort. Reference:
// Stanley Fung, Is this the simplest (and most surprising) sorting algorithm ever?, 2021.
void sort_simple(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch; (void)rs;
    if (n < 2) return;
    for (u32 k = 0; k < n; ++k) {
        for (u32 j = 0; j < n; ++j) {
            if (data[k] < data[j]) {
                SWAP_u32(data[k], data[j]);
            }
        }
    }
}

void sort_bubble(u32* data, u32 n, RandState* rs, void* scratch) {
    (void)scratch; (void)rs;
    if (n < 2) return;
    bool swapped = false;
    do {
        swapped = false;
        for (u32 k = 0; k < n - 1; ++k) {
            if (data[k] > data[k+1]) {
                SWAP_u32(data[k], data[k+1]);
                swapped = true;
            }
        }
    } while (swapped);
}

void sort_selection(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch; (void)rs;
    if (n < 2) return;
    for (u32 k = 0; k < n-1; ++k) {
        u32 least_idx = k;
        u32 least = data[k];
        for (u32 j = k+1; j < n; ++j) {
            if (data[j] < least) {
                least = data[j];
                least_idx = j;
            }
        }
        data[least_idx] = data[k];
        data[k] = least;
    }
}

void sort_insertion(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch; (void)rs;
    if (n < 2) return;
    for (u32 k = 1; k < n; ++k) {
        u32 item = data[k];
        u32 j = k;
        while ((j > 0) && (data[j-1] > item)) {
            data[j] = data[j-1];
            --j;
        }
        data[j] = item;
    }
}

void sort_gnome(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch; (void)rs;
    if (n < 2) return;
    u32 k = 0;
    while (k < n) {
        if (k == 0 || data[k] >= data[k-1]) {
            ++k;
        } else {
            SWAP_u32(data[k], data[k-1]);
            --k;
        }
    }
}

// WARNING Recursive: may cause stack overflow.
void sort_quick_(u32* data, u32 n)
{
    u32* data_last = data + n - 1;
    u32* front = data;
    u32* back = data_last;
    u32 pivot = *back;
    while (front < back) {
        if (*front < pivot) {
            ++front;
        } else {
            *back = *front;
            *front = *(back-1);
            *(back-1) = pivot;
            --back;
        }
    }
    if (front - data > 1) {
        sort_quick_(data, (u32)(front - data));
    }
    if (data_last - front > 1) {
        sort_quick_(front + 1, (u32)(data_last - front));
    }
}

// WARNING Recursive: may cause stack overflow.
void sort_quick(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch; (void)rs;
    if (n < 2) return;
    sort_quick_(data, n);
}

void sort_intro_(u32* data, u32 n, u32 max_recurse)
{
    if (n < 16) {
        // Delegate.
        sort_insertion(data, n, NULL, NULL);
        return;
    }
    if (max_recurse == 0) {
        // Delegate.
        sort_heap(data, n, NULL, NULL);
        return;
    }
    u32* data_last = data + n - 1;
    u32* front = data;
    u32* back = data_last;
    u32 pivot = *back;
    while (front < back) {
        if (*front < pivot) {
            ++front;
        } else {
            *back = *front;
            *front = *(back-1);
            *(back-1) = pivot;
            --back;
        }
    }
    if (front - data > 1) {
        sort_intro_(data, (u32)(front - data), max_recurse - 1);
    }
    if (data_last - front > 1) {
        sort_intro_(front + 1, (u32)(data_last - front), max_recurse - 1);
    }
}

void sort_intro(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch; (void)rs;
    if (n < 2) return;
    u32 max_recurse = 0;
    for (int k = n; k > 0; k /= 2) {
        ++max_recurse;
    }
    max_recurse *= 2;  // Approximately 2 * log_2 (n).
    sort_intro_(data, n, max_recurse);
}

// WARNING Recursive: may cause stack overflow.
void sort_quickr_(u32* data, u32 n, RandState* rs)
{
    u32* data_last = data + n - 1;
    u32* front = data;
    u32* back = data_last;
    // Don't waste time for tiny lists.
    if (n >= 8) {
        u32 pivot_idx = rand_range_unif(rs, 0, n-1);
        SWAP_u32(data[pivot_idx], *back);
    }
    u32 pivot = *back;
    bool constant_data = true;
    while (front < back) {
        if (*front != pivot) {
            constant_data = false;
        }
        if (*front < pivot) {
            ++front;
        } else {
            *back = *front;
            *front = *(back-1);
            *(back-1) = pivot;
            --back;
        }
    }
    if (constant_data) {
        // Avoid quadratic slowdown.
        front = data + ((data_last - data)/2);
    }
    if (front - data > 1) {
        sort_quickr_(data, (u32)(front - data), rs);
    }
    if (data_last - front > 1) {
        sort_quickr_(front + 1, (u32)(data_last - front), rs);
    }
}

// WARNING Recursive: may cause stack overflow.
void sort_quickr(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch;
    if (n < 2) return;
    sort_quickr_(data, n, rs);
}

u32 sort_shell_tokuda_gap(u32 k)
{
    u32 gaps[5] = {1, 4, 9, 20, 46};
    if (k < 5) {
        return gaps[k];
    } else {
        f32 pwr = 2.25f;
        for (u32 i = 0; i < k; ++i) {
            pwr *= 2.25f;
        }
        return (u32)(1.f + (pwr - 1.f) / 1.25f);
    }
}

void sort_shell(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch; (void)rs;
    if (n < 2) return;
    u32 gapk = 0;
    while (sort_shell_tokuda_gap(gapk) < n) { ++gapk; }
    do {
        u32 gap = sort_shell_tokuda_gap(--gapk);
        for (u32 k = gap; k < n; ++k) {
            u32 item = data[k];
            u32 j = k;
            while ((j >= gap) && (data[j - gap] > item)) {
                data[j] = data[j - gap];
                j -= gap;
            }
            data[j] = item;
         }
    } while (gapk > 0);
}

u64 sort_merge_scratch_size(u32 n)
{
    return sizeof(u32) * n;
}

void sort_merge(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)rs;
    if (n < 2) return;
    u32* src = data;
    u32* dst = (u32*)scratch;
    u32 half = 1;
    u32 stride = 2;
    u32 stride_max = n * 2;
    while (stride < stride_max) {
        u32* srcb = src + n;
        u32* dstb = dst + n;
        for (u32 k = 0; k < n; k += stride) {
            // Merge the two halves of src[k..k+stride-1] into dst[k..k+stride-1].
            u32* l = src + k;
            u32* lb = l + MIN(srcb - l, (ptrdiff_t)half);
            u32* r = lb;
            u32* rb = r + MIN(srcb - r, (ptrdiff_t)half);
            u32* d = dst + k;
            u32* db = d + MIN(dstb - d, (ptrdiff_t)stride);
            while (d < db) {
                if (r == rb) {
                    *(d++) = *(l++);
                } else if (l == lb) {
                    *(d++) = *(r++);
                } else if (*l < *r) {
                    *(d++) = *(l++);
                } else {
                    *(d++) = *(r++);
                }
            }
        }
        half = stride;
        stride *= 2;
        // Swap buffers.
        u32* tmp = src;
        src = dst;
        dst = tmp;
    }
    if (src != data) {
        // Sorted array is in wrong buffer; copy it to data.
        u32* end = src + n;
        while (src < end) {
            *(data++) = *(src++);
        }
    }
}

// Repair a damaged max heap by sifting the given element down to its correct place.
void siftdown(u32* data, u32 siftee, u32 end)
{
    u32 data_siftee = data[siftee];
    for (;;) {
        u32 dest = 2*siftee + 1;  // Left child of siftee.
        if (dest >= end) {
            break;
        }
        if ((dest + 1 < end) && (data[dest] < data[dest+1])) {
            // The right child is larger, so sift rightwards instead.
            ++dest;
        }
        if (data_siftee < data[dest]) {
            // Sift down the tree by one level. No need to write into the child; it will be
            // written during the next iteration.
            data[siftee] = data[dest];
            siftee = dest;
        } else {
            // Done sifting; this is the lowest it will go.
            break;
        }
    }
    data[siftee] = data_siftee;
}

// Rearrange the elements of the given array into a max heap. In-place.
void maxheap(u32* data, u32 n)
{
    if (n < 2) return;
    // Begin with the parent of the last element in the heap.
    for (u32 siftee = n/2; siftee > 0; ) {
        siftdown(data, --siftee, n);
    }
}

void sort_heap(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch; (void)rs;
    if (n < 2) return;
    maxheap(data, n);
    do {
        --n;
        SWAP_u32(data[0], data[n]);
        siftdown(data, 0, n);
    } while (n > 1);
}

void sort_broken(u32* data, u32 n, RandState* rs, void* scratch)
{
    f32 chance_of_failure = 0.01f;
    sort_heap(data, n, rs, scratch);
    if ((n > 1) && rand_bernoulli(rs, chance_of_failure)) {
        // After sorting, these are the two elements most likely to be distinct.
        SWAP_u32(data[0], data[n-1]);
    }
}

void sort_miracle(u32* data, u32 n, RandState* rs, void* scratch)
{
    (void)scratch; (void)rs;
    bool sorted = false;
    do {
        sorted = true;
        for (u32 i = 1; i < n; ++i) {
            if (data[i-1] > data[i]) {
                sorted = false;
                continue;
            }
        }
    } while (!sorted);
}


/**** Verifiers ****/

bool verify_ordered(u32* input, u32* output, u32 n, RandState* rs, void* scratch)
{
    (void)rs; (void)scratch; (void)input;
    if (n < 2) {
        return true;
    }
    for (u32 k = 0; k < n-1; ++k) {
        if (output[k] > output[k+1]) {
            return false;
        }
    }
    return true;
}

u64 verify_checksum_(u32* data, u32 n)
{
    u32 checksum_xor = 0;
    u32 checksum_add = 0;
    for (u32 k = 0; k < n; ++k) {
        checksum_xor ^= data[k];
        checksum_add += data[k];
    }
    u64 checksum =
        ((u64)checksum_xor << 32) +
        (u64)checksum_add;
    checksum = ROT64(checksum, n % 64) + n;
    return checksum;
}

bool verify_checksum(u32* input, u32* output, u32 n, RandState* rs, void* scratch)
{
    (void)rs; (void)scratch;
    return verify_checksum_(input, n) == verify_checksum_(output, n);
}

bool verify_all(u32* input, u32* output, u32 n, RandState* rs, void* scratch)
{
    return
        verify_checksum(input, output, n, rs, scratch) &&
        verify_ordered(input, output, n, rs, scratch);
}
