#include <iostream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <random>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>

#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

// ==========================================
// 0. 底层计时与配置
// ==========================================
[[nodiscard]] __attribute__((always_inline)) 
inline uint64_t get_cycles() {
    unsigned int dummy;
    return __rdtscp(&dummy); 
}

const size_t CAP_POW2 = 1 << 21; // 2M 容量 (2097152)
const int QUERY_SAMPLES = 200000; // 采样次数

// ==========================================
// 1.  Robin Hood Map
// ==========================================
struct Bucket {
    uint64_t key;
    uint64_t value;
    int16_t psl;
    bool occupied;
};

class BenchRHH {
    Bucket* table_;
    size_t cap_;
    size_t mask_; // 缓存掩码
    size_t size_ = 0;

public:
    explicit BenchRHH(size_t c) {
        cap_ = c;
        mask_ = cap_ - 1;
        // 使用 calloc 申请归零页，比 new[] 快且利用 OS 惰性初始化
        table_ = static_cast<Bucket*>(std::calloc(cap_, sizeof(Bucket)));
    }

    ~BenchRHH() { std::free(table_); }

    void insert(uint64_t k, uint64_t v) {
        Bucket curr{k, v, 0, true};
        size_t idx = hash(k) & mask_; // 位运算替代取模

        for (size_t probes = 0; probes < cap_; ++probes) {
            Bucket& entry = table_[idx];
            if (!entry.occupied) {
                entry = curr;
                size_++;
                return;
            }
            if (entry.key == k) return;

            // Robin Hood 核心交换
            if (curr.psl > entry.psl) {
                std::swap(curr, entry);
            }
            curr.psl++;
            idx = (idx + 1) & mask_;
        }
    }

    [[nodiscard]] bool find(uint64_t k, uint64_t& v) const {
        size_t idx = hash(k) & mask_;
        int16_t d = 0;
        while (true) {
            const Bucket& entry = table_[idx];
            // 提前终止优化
            if (!entry.occupied || d > entry.psl) return false;
            if (entry.key == k) {
                v = entry.value;
                return true;
            }
            d++;
            idx = (idx + 1) & mask_;
        }
    }
    
    static inline uint64_t hash(uint64_t k) {
        k ^= k >> 33; k *= 0xff51afd7ed558ccd; k ^= k >> 33; return k;
    }
};

// ==========================================
// 2. 测试逻辑
// ==========================================
struct Result {
    uint64_t p50;
    uint64_t p99;
    uint64_t p999;
    uint64_t max;
};

Result run_test_std(const std::vector<uint64_t>& keys, const std::vector<uint64_t>& queries) {
    std::unordered_map<uint64_t, uint64_t> m;
    m.reserve(CAP_POW2); 
    for(auto k : keys) m[k] = k;

    std::vector<uint64_t> stats;
    stats.reserve(queries.size());
    uint64_t dummy = 0;

    // 预热 Cache
    for(int i=0; i<1000; ++i) dummy += m.count(queries[i]);

    for(auto k : queries) {
        uint64_t start = get_cycles();
        auto it = m.find(k);
        uint64_t end = get_cycles();
        
        if (it != m.end()) asm volatile("" : "+r"(dummy) : "r"(it->second)); 
        
        uint64_t lat = end - start;
        if (lat < 100000) stats.push_back(lat);
    }

    std::sort(stats.begin(), stats.end());
    size_t n = stats.size();
    return {stats[n*0.5], stats[n*0.99], stats[n*0.999], stats.back()};
}

Result run_test_rhh(const std::vector<uint64_t>& keys, const std::vector<uint64_t>& queries) {
    BenchRHH rhh(CAP_POW2);
    for(auto k : keys) rhh.insert(k, k);

    std::vector<uint64_t> stats;
    stats.reserve(queries.size());
    uint64_t val;
    
    // 预热 Cache
    for(int i=0; i<1000; ++i) rhh.find(queries[i], val);

    for(auto k : queries) {
        uint64_t start = get_cycles();
        bool found = rhh.find(k, val);
        uint64_t end = get_cycles();
        
        if (found) asm volatile("" : "+r"(val)); 
        
        uint64_t lat = end - start;
        if (lat < 100000) stats.push_back(lat);
    }

    std::sort(stats.begin(), stats.end());
    size_t n = stats.size();
    return {stats[n*0.5], stats[n*0.99], stats[n*0.999], stats.back()};
}

// ==========================================
// 3. 主程序
// ==========================================
int main() {
    std::vector<double> load_factors = {0.50, 0.75, 0.90, 0.95, 0.99};

    std::cout << "\n========================================================================================\n";
    std::cout << "  Robin Hood Hashing vs std::unordered_map (Cycles P99 Latency)\n";
    std::cout << "  Environment: " << CAP_POW2 << " Capacity, " << QUERY_SAMPLES << " Queries (80% Hit / 20% Miss)\n";
    std::cout << "========================================================================================\n";
    std::cout << std::left 
              << std::setw(10) << "Load(%)" 
              << std::setw(15) << "STD P99" 
              << std::setw(15) << "RHH P99" 
              << std::setw(15) << "Improvement"
              << std::setw(15) << "RHH P99.9"
              << "Note" << std::endl;
    std::cout << "----------------------------------------------------------------------------------------\n";

    for (double alpha : load_factors) {
        // 1. 生成数据
        size_t target_size = static_cast<size_t>(CAP_POW2 * alpha);
        std::mt19937_64 rng(42);
        std::vector<uint64_t> keys;
        keys.reserve(target_size);
        for(size_t i=0; i<target_size; ++i) keys.push_back(rng());

        std::vector<uint64_t> queries;
        queries.reserve(QUERY_SAMPLES);
        for(int i=0; i<QUERY_SAMPLES; ++i) {
            if ((rng() & 0xFF) < 204) queries.push_back(keys[rng() % keys.size()]); // 80% Hit
            else queries.push_back(rng() + 1); // 20% Miss
        }

        // 2. 运行测试
        Result res_std = run_test_std(keys, queries);
        Result res_rhh = run_test_rhh(keys, queries);

        // 3. 计算提升
        double diff = (double)res_std.p99 - (double)res_rhh.p99;
        double improvement = 100.0 * diff / (double)res_std.p99;
        // 4. 输出行
        std::string note = "";
        if (alpha >= 0.90 && improvement > 50.0) note = "<-- peak gain range";

        std::cout << std::left 
                  << std::setw(10) << (alpha * 100) 
                  << std::setw(15) << res_std.p99 
                  << std::setw(15) << res_rhh.p99 
                  << std::fixed << std::setprecision(1) << improvement << "%" << std::setw(9) << ""
                  << std::setw(15) << res_rhh.p999
                  << note << std::endl;
    }
    std::cout << "========================================================================================\n";
    std::cout << "* Unit: CPU Cycles (Approx 0.3ns per cycle on 3GHz CPU)\n";

    return 0;
}