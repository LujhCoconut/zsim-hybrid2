#ifndef _MC_H_
#define _MC_H_

#include "config.h"
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include <string>
#include "stats.h"
#include "g_std/g_unordered_map.h"
#include "g_std/g_vector.h"
// #include <unordered_map>

#define MAX_STEPS 10000

enum ReqType
{
	LOAD = 0,
	STORE
};

enum Scheme
{
   AlloyCache,
   UnisonCache,
   HMA,
   HybridCache,
   NoCache,
   CacheOnly,
   Tagless,
   Hybrid2
};

class Way
{
public:
   Address tag;
   bool valid;
   bool dirty;
};

class Set
{
public:
   Way * ways;
   uint32_t num_ways;

   uint32_t getEmptyWay()
   {
      for (uint32_t i = 0; i < num_ways; i++)
         if (!ways[i].valid)
            return i;
      return num_ways;
   };
   bool hasEmptyWay() { return getEmptyWay() < num_ways; };
};

// Not modeling all details of the tag buffer. 
class TagBufferEntry
{
public:
	Address tag;
	bool remap;
	uint32_t lru;
};

class TagBuffer : public GlobAlloc {
public:
	TagBuffer(Config &config);
	// return: exists in tag buffer or not.
	uint32_t existInTB(Address tag);
	uint32_t getNumWays() { return _num_ways; };

	// return: if the address can be inserted to tag buffer or not.
	bool canInsert(Address tag);
	bool canInsert(Address tag1, Address tag2);
	void insert(Address tag, bool remap);
	double getOccupancy() { return 1.0 * _entry_occupied / _num_ways / _num_sets; };
	void clearTagBuffer();
	void setClearTime(uint64_t time) { _last_clear_time = time; };
	uint64_t getClearTime() { return _last_clear_time; };
private:
	void updateLRU(uint32_t set_num, uint32_t way);
	TagBufferEntry ** _tag_buffer;
	uint32_t _num_ways;
	uint32_t _num_sets;
	uint32_t _entry_occupied;
	uint64_t _last_clear_time;
};

class TLBEntry
{
public:
   uint64_t tag;
   uint64_t way;
   uint64_t count; // for OS based placement policy

   // the following two are only for UnisonCache
   // due to space cosntraint, it is not feasible to keep one bit for each line, 
   // so we use 1 bit for 4 lines.
   uint64_t touch_bitvec; // whether a line is touched in a page
   uint64_t dirty_bitvec; // whether a line is dirty in page
};

class LinePlacementPolicy;
class PagePlacementPolicy;
class OSPlacementPolicy;

//class PlacementPolicy;
class DDRMemory;

class MemoryController : public MemObject {
private:
	DDRMemory * BuildDDRMemory(Config& config, uint32_t frequency, uint32_t domain, g_string name, const std::string& prefix, uint32_t tBL, double timing_scale);
	
	g_string _name;

	// Trace related code
	lock_t _lock;
	bool _collect_trace;
	g_string _trace_dir;
	Address _address_trace[10000];
	uint32_t _type_trace[10000];
	uint32_t _cur_trace_len;
	uint32_t _max_trace_len;

	// External Dram Configuration
	MemObject *	_ext_dram;
	g_string _ext_type; 
public:	
	// MC-Dram Configuration
	MemObject ** _mcdram;
	uint32_t _mcdram_per_mc;
	g_string _mcdram_type;
	// ----------------------------------------------------------
	//【newAddition】 新增Hybrid2。
	// DDRMemory * test_mem;

	MemObject ** _cachehbm;
	uint32_t _cache_hbm_per_mc;
	g_string _cache_hbm_type;
	uint32_t _cache_hbm_size;

	MemObject ** _memhbm;
	uint32_t _mem_hbm_per_mc;
	g_string _mem_hbm_type;
	uint32_t _mem_hbm_size;
	uint64_t hbm_set_num;


	uint32_t set_assoc_num;
	uint32_t _hybrid2_page_size;
	uint32_t _hybrid2_blk_size;
	uint32_t reserved_memory_size = 1024*1024*1024; //测试1GB 似乎没问题

	// 1GB -> (2KB PAGE) 512Pages 8pages->1set (非顺序) 512/8 = 64pages
	uint64_t hbm_pages_per_set; 
	// uint64_t dram

	uint32_t hybrid2_blk_per_page;
	// Hybrid2论文的XTA
	// 论文中包括缓存必要字段tag,LRUstate,有效标记，脏标记
	struct XTAEntry{
		uint64_t _hybrid2_tag;
		uint64_t _hybrid2_LRU;
		// 不在这里固定数组大小
		// uint64_t* bit_vector;
		// uint64_t* dirty_vector;

		// 测试一下写死会不会规避一些问题,似乎能规避一些问题，但是还是原来的问题
		uint64_t bit_vector[64];
		uint64_t dirty_vector[64];

		// int bit_vector[hybrid2_blk_per_page];
		// int dirty_vector[hybrid2_blk_per_page];
		uint64_t _hybrid2_counter;
		uint64_t _hbm_tag;
		uint64_t _dram_tag;
		XTAEntry() : _hybrid2_counter(0) {}

		//  ~XTAEntry() {
		// 	// 释放动态分配的内存
		// 	if (bit_vector != nullptr) {
		// 		delete[] bit_vector;  // 释放 bit_vector 所指向的内存
		// 	}
		// 	if (dirty_vector != nullptr) {
		// 		delete[] dirty_vector;  // 释放 dirty_vector 所指向的内存
		// 	}
    	// }
	};

	// 在这里需要初始化一个XTA，在XTA的设计中：一个Set就有一个XTAEntries，
	// 每个XTAEntries对应多个页面的XTAEntry
	// 因此最终的XTA由一个两层vector结构包含 
	g_vector<g_vector<XTAEntry>> XTA;

	// 代表内存是否被占用
	// 一个set一个SETEntries,一个set的前
	g_vector<g_vector<int>> memory_occupied;


	// 迁移映射表
	// 假设Workflow是[ XTAHit-Cacheline Miss]
	// 先查一边哈希表，没有直接根据地址范围判断介质，有的话根据哈希表返回地址判断介质
	// 再返回延迟。
	// 这个MTable是否要设置容量呢？不用，根据内存访问erase没用的应该就行
	// 需要更仔细地考虑合理性
	g_unordered_map<uint64_t,uint64_t> HBMTable;
	g_unordered_map<uint64_t,uint64_t> DRAMTable;

	uint64_t get_set_id(uint64_t addr);
	uint64_t get_page_id(uint64_t addr);
	g_vector<XTAEntry>& find_XTA_set(uint64_t set_id);
	uint64_t ret_lru_page(g_vector<XTAEntry> SETEntries);
	int check_set_full(g_vector<XTAEntry> SETEntries);
	Address vaddr_to_paddr(MemReq req);
	Address paddr_to_vaddr(Address pLineAddr);
	Address handle_low_address(Address addr);
	bool is_hbm(MemReq req);
	uint64_t random_hybrid2_access(MemReq req);
	uint64_t hbm_hybrid2_access(MemReq req);
	// 不使用页表和MMU TLB进行地址转换，简单采用模运算进行固定映射
	uint64_t phy_mem_size;
	// uint64_t page_size;
	uint64_t num_pages;
	g_unordered_map<uint64_t,uint64_t> fixedMapping;

	


	// ----------------------------------------------------------

	uint64_t getNumRequests() { return _num_requests; };
   	uint64_t getNumSets()     { return _num_sets; };
   	uint32_t getNumWays()     { return _num_ways; };
   	double getRecentMissRate(){ return (double) _num_miss_per_step / (_num_miss_per_step + _num_hit_per_step); };
   	Scheme getScheme()      { return _scheme; };
   	Set * getSets()         { return _cache; };
   	g_unordered_map<Address, TLBEntry> * getTLB() { return &_tlb; };
	TagBuffer * getTagBuffer() { return _tag_buffer; };

	uint64_t getGranularity() { return _granularity; };

private:
	// For Alloy Cache.
	Address transMCAddress(Address mc_addr);
	// For Page Granularity Cache
	Address transMCAddressPage(uint64_t set_num, uint32_t way_num); 

	// For Tagless.
	// For Tagless, we don't use "Set * _cache;" as other schemes. Instead, we use the following 
	// structure to model a fully associative cache with FIFO replacement 
	//vector<Address> _idx_to_address;
	uint64_t _next_evict_idx;
	//map<uint64_t, uint64_t> _address_to_idx;

	// Cache structure
	uint64_t _granularity;
	uint64_t _num_ways;
	uint64_t _cache_size;  // in Bytes
	uint64_t _num_sets;
	Set * _cache;
	LinePlacementPolicy * _line_placement_policy;
	PagePlacementPolicy * _page_placement_policy;
	OSPlacementPolicy * _os_placement_policy;
	uint64_t _num_requests;
	Scheme _scheme; 
	TagBuffer * _tag_buffer;
	
	// For HybridCache
	uint32_t _footprint_size; 

	// Balance in- and off-package DRAM bandwidth. 
	// From "BATMAN: Maximizing Bandwidth Utilization of Hybrid Memory Systems"
	bool _bw_balance; 
	uint64_t _ds_index;

	// TLB Hack
	g_unordered_map <Address, TLBEntry> _tlb;
	uint64_t _os_quantum;

    // Stats
	Counter _numPlacement;
  	Counter _numCleanEviction;
	Counter _numDirtyEviction;
	Counter _numLoadHit;
	Counter _numLoadMiss;
	Counter _numStoreHit;
	Counter _numStoreMiss;
	Counter _numCounterAccess; // for FBR placement policy  

	Counter _numTagLoad;
	Counter _numTagStore;
	// For HybridCache	
	Counter _numTagBufferFlush;
	Counter _numTBDirtyHit;
	Counter _numTBDirtyMiss;
	// For UnisonCache
	Counter _numTouchedLines;
	Counter _numEvictedLines;

	uint64_t _num_hit_per_step;
   	uint64_t _num_miss_per_step;
	uint64_t _mc_bw_per_step;
	uint64_t _ext_bw_per_step;
   	double _miss_rate_trace[MAX_STEPS];

   	uint32_t _num_steps;

	// to model the SRAM tag
	bool 	_sram_tag;
	uint32_t _llc_latency;
public:
	MemoryController(g_string& name, uint32_t frequency, uint32_t domain, Config& config);
	uint64_t hybrid2_access(MemReq& req);
	uint64_t access(MemReq& req);
	const char * getName() { return _name.c_str(); };
	void initStats(AggregateStat* parentStat); 
	// Use glob mem
	//using GlobAlloc::operator new;
	//using GlobAlloc::operator delete;
};

#endif
