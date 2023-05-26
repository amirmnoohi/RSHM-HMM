#include <linux/init.h>    // Default macros for __init and __exit functions
#include <linux/module.h>  // module functions
#include <linux/device.h>  // Needed to support kernel driver module
#include <linux/kernel.h>  // Debug macros
#include <linux/fs.h>      // Linux file systems support
#include <linux/uaccess.h> // Needed for 'copy_to_user'/'copy_from_user' functions
#include <crypto/hash.h>   // Support to hashing cryptographic functions
#include <linux/slab.h>    // In-kernel dynamic memory allocation
#include <linux/types.h>   // Required for custom data types
#include <linux/mm.h>
#include <linux/crypto.h>
#include <linux/types.h>
#include <linux/string.h>

#define XXH3_64BIT_DEFAULT_SEED 0x0ULL

static inline u64 XXH3_rotl64(u64 x, int r) {
    return (x << r) | (x >> (64 - r));
}

static u64 XXH3_avalanche(u64 h64) {
    h64 ^= h64 >> 37;
    h64 *= 0x165667919E3779F9ULL;
    h64 ^= h64 >> 32;
    return h64;
}

u64 XXH3_64bit(const char* input) {
    const u8* p = (const u8*)input;
    const u8* const bEnd = p + strlen(input);
    u64 acc = XXH3_64BIT_DEFAULT_SEED;

    if (strlen(input) >= 32) {
        const u8* const limit = bEnd - 32;
        do {
            u64 const k1 = *(const u64*)p;
            u64 const k2 = *(const u64*)(p + 8);
            u64 const k3 = *(const u64*)(p + 16);
            u64 const k4 = *(const u64*)(p + 24);

            acc = XXH3_rotl64(acc ^ (k1 * 0x51B051B051B051BULL), 29) + (k2 * 0x7CB07CB07CB07CB1ULL);
            acc = XXH3_rotl64(acc, 21);
            acc = acc * 0xC6A4A7935BD1E995ULL + k3;
            acc = XXH3_rotl64(acc, 19) ^ (k4 * 0x51B051B051B051BULL);

            p += 32;
        } while (p <= limit);
    }

    const u8* const limit = bEnd - 16;
    while (p <= limit) {
        u64 const k1 = *(const u64*)p;
        u64 const k2 = *(const u64*)(p + 8);

        acc = XXH3_rotl64(acc ^ (k1 * 0x9FB21C651E98DF25ULL), 31) * 0x9FB21C651E98DF25ULL + (k2 * 0x9FB21C651E98DF25ULL);

        p += 16;
    }

    if (p < bEnd) {
        u64 const k1 = *(const u64*)p;

        acc = XXH3_rotl64(acc ^ (k1 * 0xA0761D6478BD642FULL), 27) * 0xA0761D6478BD642FULL;
    }

    // Finalize
    acc ^= strlen(input);
    acc = XXH3_avalanche(acc);

    return acc;
}

// Define a static function named "nsm_init" that returns an integer value
static int __init rshm_init(void)
{

    ktime_t start, end;
    struct crypto_shash *algorithm;
    struct shash_desc *desc;
    int err, i, j;
    u64 total_time;
    s64 average_time, diff;
    // uint64_t hash;
    char data[100];
    char digest[200];

    strcpy(data, "Hello World!");

    // check if algorithm exist

    algorithm = crypto_alloc_shash("md5", 0, 0);
    if (IS_ERR(algorithm))
    {
        pr_info("Error Algorithm: %ld\n", PTR_ERR(algorithm));
        return 0;
    }

    desc = kmalloc(sizeof(struct shash_desc) + crypto_shash_descsize(algorithm), GFP_KERNEL);

    desc->tfm = algorithm;

    // Initialize shash API
    err = crypto_shash_init(desc);

    total_time = 0;
    for (i = 1; i <= 1000000; i *= 10)
    {
        for (j = 0; j < i; j++)
        {
            // clear data and digest
            memset(data, 0, 100);
            memset(digest, 0, 200);

            sprintf(data, "Iteration: %d", j + 1); // Change the string in each iteration
            start = ktime_get();

            // // Calculate the XXH3 64-bit hash
            // hash = XXH3_64bit(data);

            // Execute hash function
            err = crypto_shash_update(desc, data, strlen(data));

            // Write the result to a new char buffer
            err = crypto_shash_final(desc, digest);

            end = ktime_get();

            diff = ktime_to_ns(ktime_sub(end, start));
            total_time += diff;
        }
        average_time = total_time / i;

        pr_info("Average Hash Time for %d Iteration: %lld ns\n", i, average_time);
        // pr_info("Original: %s | Hashed: %llu\n", data, hash);

    }

    crypto_free_shash(algorithm);
    kfree(desc);

    return 0;
}

// Define a static function named "nsm_exit" that doesn't return anything
static void __exit rshm_exit(void)
{
    pr_notice("RSHM Fully Unloaded\n");
}

module_init(rshm_init);
module_exit(rshm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AMIRMNOOHI");
MODULE_DESCRIPTION("HASH Benchmarking");
