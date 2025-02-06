#ifndef _MC_H_
#define _MC_H_

#include "config.h"
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include <string>
#include "stats.h"
#include "g_std/g_unordered_map.h"
#include "g_std/g_vector.h"
#include "g_std/g_list.h"
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
   Hybrid2,
   Chameleon,
   Bumblebee,
   CacheMode,
   DirectFlat
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
	uint64_t _cycle_trace[10000];
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
	//Hybrid2[HPCA'20] Reproduce
	// DDRMemory * test_mem;
	// uint64_t tag_probe_latency;

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

	uint64_t chbm_miss_cntr = 0;
	uint64_t cntr_last_cycle = 10;
	const static int time_intv = 100000; //100K
	uint32_t hybrid2_blk_per_page;
	// Hybrid2论文的XTA
	// 论文中包括缓存必要字段tag,LRUstate,有效标记，脏标记
	struct XTAEntry{
		uint64_t _hybrid2_tag;
		uint64_t _hybrid2_LRU;
		// 不在这里固定数组大小
		// uint64_t* bit_vector;
		// uint64_t* dirty_vector;

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
	// 将元素从cacheline地址修改为page地址 update[2024/12/30]
	g_unordered_map<uint64_t,uint64_t> HBMTable;
	g_unordered_map<uint64_t,uint64_t> DRAMTable;

	uint64_t get_set_id(uint64_t addr);
	uint64_t get_page_id(uint64_t addr);
	g_vector<XTAEntry>& find_XTA_set(uint64_t set_id);
	uint64_t ret_lru_page(g_vector<XTAEntry> SETEntries);
	int check_set_full(g_vector<XTAEntry> SETEntries);
	int check_set_occupy(g_vector<XTAEntry> SETEntries);
	Address vaddr_to_paddr(MemReq req);
	Address paddr_to_vaddr(Address pLineAddr);
	Address handle_low_address(Address addr);
	bool is_hbm(MemReq req);
	uint64_t random_hybrid2_access(MemReq req);
	uint64_t hbm_hybrid2_access(MemReq req);
	// 不使用页表和MMU TLB进行地址转换，简单采用模运算进行固定映射
	uint32_t phy_mem_size;
	// uint64_t page_size;
	uint64_t num_pages;
	g_unordered_map<uint64_t,uint64_t> fixedMapping;

	
    // ----------------------------------------------------------
	// Chameleon[MICRO'18] Reproduce
	// some parameters using the same parameters in hybrid2'
	uint32_t _chameleon_blk_size;
	// SRRT: Segment Restricted Remapping Tables => track the hardware remapped segments
	// Here, assume the lower address range is HBM : Segment0 <=> HBM ; Others <=> DDR
	struct segGrpEntry
	{
		// in gem5 , addr is defined, but it seems taht no need to do so.
		int ddrNum; // segDDRNum = DDRSize / HBMSize ; Default:8
		bool cacheMode; // false:POM True:Cache
		bool dirty;
		// Alloc Bit Vector -> is busy or not ; 
		g_vector<bool> ABV; 
		// hotness
		uint64_t _chameleon_counter;
		
		// Similar to hash map, array index is the key which indicates current segment, 
		// the value is the v which indicates remapping segment
		// Notes again, the lower address range is HBM. 
		// Thus, an example : remapVector[0] = 1 indicates the HBM stores ddr seg[1].
		// Once access seg[1], redirect to seg[0]
		// remapVector[1] = 1 is primary state which indicates no remapping
		g_vector<int> remapVector;

		// Similar to hash map, array index is the key which indicates current segment.
		// This data struct is able to help dealing with read and writeback coherence.
		// e.g. in cache mode, read ddr segment 1 ,data should be cached in hbm segment 0
		//      > it seems that we can only use remapVector here for this instance ?
		// e.g. when turnning to POM mode, the data in HBM cache should be writeback
		//		> it also seems that we can only use remapVector here for this instance ?
		g_vector<int> coherenceVector;
		g_vector<MemReq> reqQueue;

		segGrpEntry(int _ddrNum=8, bool _cacheMode=true, bool _dirty=false):
			ddrNum(_ddrNum),cacheMode(_cacheMode),dirty(_dirty)
		{
			for(int i = 0; i< ddrNum+1; i++)
			{
				ABV.push_back(0);
				remapVector.push_back(i);
				coherenceVector.push_back(-1);
			}
		}

		bool isCache()
		{
			return cacheMode;
		}

		void setCacheMode(bool value)
		{
			cacheMode = value;
			return;
		}

		bool isDirty()
		{
			return dirty;
		}

		void setDirty(bool value)
		{
			dirty=value;
			return;
		}

		bool isSegmentBusy(int segment)
		{
			if(ABV[segment]==0) return false;
			else return true;
		}

		// bool setSegmentBusy(int segment,bool value)
		// {
		// 	ABV[segment]=value;
		// 	//
		// 	return true;
		// }

	};

	g_vector<segGrpEntry> segGrps;
	uint64_t get_segment(Address addr);
	int get_segment_num(Address addr); // hbm 0; ddr 1 - n;
	

	// ----------------------------------------------------------
	// Bumblebee[DAC'23] Reproduce

	// notes:
	// m和n的值的设计是比较讲究的，首先隐含的等式是m/n=DDRSize/HBMSize
	// 在此基础上m,n值越大，对于Footprint足够小的应用，可以减少同set HBM竞争
	// 从而将访存操作更多位于HBM上
	// 代价是：模拟器执行时间显著增加（涉及到多次O(m+n)复杂度的操作）；
	const static int bumblebee_m = 128*2; // 取消const static时 请将下面结构体的构造函数取消，并在cpp中初始化
	const static int bumblebee_n = 16*2; // paper n
	int rh_upper = 30; //Rh较高的超参数
	int bumblebee_T; // paper T
	uint32_t _bumblebee_page_size;
	uint32_t _bumblebee_blk_size;
	const static int hot_data = 32;

	const static int blk_per_page = 64;

	struct PLEEntry{
		g_vector<int> PLE; // -1 : 未分配
		g_vector<int> Occupy; // 0:Not use ; 1:Use
		g_vector<int> Type; // 0:DRAM 1:mHBM 2:cHBM (only 1 & 2 are used)

		// HBM is in the front of the set
		// ple value can be multiple, cache should be considered first !!
		PLEEntry()
		{
			for(int i = 0;i < (bumblebee_m + bumblebee_n);i++)
			{
				PLE.push_back(-1);
				Occupy.push_back(0);
				if(i < bumblebee_n)
				{
					Type.push_back(1);
				}else{
					Type.push_back(0);
				}
			}
		}
	};

	

	// BLE 数组会追踪 cHBM 和 mHBM 页面中已被访问的块
	// 如果页面中的大多数块已被预取到 cHBM，表明该页面具有强空间局部性，应该被切换为 mHBM 页面。
	// 如果大多数 HBM 页面表现出强空间局部性，则应将更多的片外页面迁移到 mHBM。
	struct BLEEntry{
		int ple_idx; // 可以索引到PLEEntry page-offset
		int cntr;
		uint64_t l_cycle;
		g_vector<int> validVector;
		g_vector<int> dirtyVector;

		BLEEntry(int _ple_idx = -1,int _cntr = 0,uint64_t _l_cycle = 0):ple_idx(_ple_idx),cntr(_cntr),l_cycle(_l_cycle)
		{
			for(int i = 0 ;i < blk_per_page; i++)
			{
				validVector.push_back(0);
				dirtyVector.push_back(0);
			}
		}
	};
           
	struct MetaGrpEntry{
		g_vector<BLEEntry> _bleEntries; // 需要被初始化
		PLEEntry _pleEntry;
		int set_alloc_page;
		// ......

		MetaGrpEntry(int alloc_pg = 0):set_alloc_page(alloc_pg)
		{
			for(int i = 0;i < bumblebee_m + bumblebee_n;i++)
			{
				BLEEntry _bleEntry;
				_bleEntry.ple_idx = i; 
				_bleEntries.push_back(_bleEntry);
			}
		}
	};

	g_vector<MetaGrpEntry> MetaGrp;
	int ret_set_alloc_state(MetaGrpEntry& set);


	struct QueuePage{
		int _page_id; // idx or page_offset ?
		uint64_t _counter;
		uint64_t _last_mod_cycle;
		QueuePage(uint64_t cntr = 0,int last_mod_cycle = 10):_counter(cntr),_last_mod_cycle(last_mod_cycle){};
	};


	// 具有高访问比例的 mHBM 页面反映了强空间局部性，
	// 而具有低访问比例的 mHBM 页面以及剩余的 cHBM 页面反映了弱空间局部性。
	// 重映射集中空间局部性程度 (SL) 的评估公式为：SL = Na − Nn − Nc
	// SL>0（强空间局部性），应将更多的热点数据迁移到 mHBM，以更好地利用空间局部性并充分利用内存带宽
	// SL≤0（弱空间局部性），应将热点数据缓存到 cHBM，以减少过度预取的情况。


	uint64_t long_time = 2000000; // 长时间的超参数


	// 时间局部性
	// 如果rh较高，对于 SL>0 只有热度值大于 T 的页面被允许迁移到 mHBM
	// 对于SL≤0 ，只有页面中热度值大于 T 的块被允许缓存到 cHBM
	struct HotnenssTracker{
		int _rh; // HBM Occupied Ratio
		int _T; //阈值
		int _nc; // number of cHBM Pages
		int _na; // mHBM accessed
		int _nn; // mHBM not accessed
		uint64_t _last_mod_cycle;
		// LRU Hot Table Queue
		g_list<QueuePage> HBMQueue; 
		g_list<QueuePage> DRAMQueue;

		HotnenssTracker(int rh = 0, int nc = bumblebee_n, int na=0, int nn= 0, uint64_t lcycle = 0):
			_rh(rh),_nc(nc),_na(na),_nn(nn),_last_mod_cycle(lcycle)
		{

		}
	};

	// 一个set 一个HotnessTracker
	g_vector<HotnenssTracker> HotnessTable;
	Address getDestAddress(uint64_t set_id,int idx,int page_offset,int blk_offset);
	void tryEvict(PLEEntry& pleEntry,HotnenssTracker& hotTracker,uint64_t current_cycle,g_vector<BLEEntry>& bleEntries,uint64_t set_id,MemReq& req);
	void tryEvict_2(PLEEntry& pleEntry,HotnenssTracker& hotTracker,uint64_t current_cycle,g_vector<BLEEntry>& bleEntries,uint64_t set_id,MemReq& req);
	void hotTrackerDecrease(HotnenssTracker& hotTracker,uint64_t current_cycle);

	std::pair<int,uint64_t> find_coldest(HotnenssTracker& hotTracker);
	std::pair<int,uint64_t> find_hottest(HotnenssTracker& hotTracker);
	// int find_coldest(HotnenssTracker& hotTracker);
	// int find_hottest(HotnenssTracker& hotTracker);
	int ret_hbm_occupy(MetaGrpEntry& set);
	void trySwap(PLEEntry& pleEntry,HotnenssTracker& hotTracker,MetaGrpEntry& set,uint64_t current_cycle,uint64_t set_id,MemReq& req);

	bool shouldPop(HotnenssTracker& hotTracker);
	// 缓存异步执行的迁移的队列
	struct AsynReq{ //未设计初始化函数，使用此数据结构请务必正确初始化
		MemReq _asynReq;
		int type; // 交给哪一种内存介质进行处理 0->HBM 1->DDR
		int channel_select; // 如果是HBM处理，需要的通道选择参数
		int access_type; // 0 立即执行 1：关键路径稍后执行 2：非关键路径稍后执行
	};

	g_list<AsynReq> AsynReqQueue; // 尾部加入push_back 头部弹出pop_front
	void execAsynReq(); // 每次Access完都会清空AsynReqQueue,解决了happens-before的问题
	lock_t _AsynQueuelock; // 只有一个线程可以修改AsynReqQueue(多生产者多消费者模型)

	void hotTrackerState(HotnenssTracker& hotTracker,PLEEntry& pleEntry);

	// -----------------------------DCache Configurations-----------------------------


	// -------------------------------End DCache Configurations-------------------------------

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
	// 小于ds_index的cache（HBM）则被标记为disable，未被使用
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
	uint64_t chameleon_access(MemReq& req);
	uint64_t bumblebee_access(MemReq& req);
	uint64_t hybrid2_access(MemReq& req);
	uint64_t direct_flat_access(MemReq& req);
	uint64_t access(MemReq& req);
	const char * getName() { return _name.c_str(); };
	void initStats(AggregateStat* parentStat); 
	// Use glob mem
	//using GlobAlloc::operator new;
	//using GlobAlloc::operator delete;
};

#endif
