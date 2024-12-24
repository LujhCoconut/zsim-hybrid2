#include "mc.h"
#include "line_placement.h"
#include "page_placement.h"
#include "os_placement.h"
#include "mem_ctrls.h"
#include "dramsim_mem_ctrl.h"
#include "ddr_mem.h"
#include "zsim.h"
#include <algorithm>
#include <iostream>
#include <cmath>
#include <random>

/**
 * Complete the code. Pass the basic test.
 * There are stll somed details to be considered:
 * 1) Should MESI states change again when they come into function ?
 * 2) Bit Computing should be token into consideration later.
 * 3) Lots of operation has been invovled into `looking up hash table`, which perhaps making the performance slowdown.
 *
 * Although this project can run smoothly, there still a problem I can not understand:
 * 1) Why _memhbm occurs INVALID-ADDRESS-ACCESS bug, while _mcdram does not. They are almost the same.\
 *
 */
MemoryController::MemoryController(g_string &name, uint32_t frequency, uint32_t domain, Config &config)
	: _name(name)
{
	// Trace Related
	_collect_trace = config.get<bool>("sys.mem.enableTrace", false);
	if (_collect_trace && _name == "mem-0")
	{
		_cur_trace_len = 0;
		_max_trace_len = 10000;
		_trace_dir = config.get<const char *>("sys.mem.traceDir", "./");
		// FILE * f = fopen((_trace_dir + g_string("/") + _name + g_string("trace.bin")).c_str(), "wb");
		FILE *f = fopen((_trace_dir + _name + g_string("trace.txt")).c_str(), "w"); // 新增
		// uint32_t num = 0;
		fprintf(f, "cycle, address, type\n"); // 新增
		// fwrite(&num, sizeof(uint32_t), 1, f);
		fclose(f);
		futex_init(&_lock);
	}
	_sram_tag = config.get<bool>("sys.mem.sram_tag", false);
	_llc_latency = config.get<uint32_t>("sys.caches.l3.latency");
	double timing_scale = config.get<double>("sys.mem.dram_timing_scale", 1);
	g_string scheme = config.get<const char *>("sys.mem.cache_scheme", "NoCache");
	_ext_type = config.get<const char *>("sys.mem.ext_dram.type", "Simple");
	if (scheme != "NoCache" && scheme != "Hybrid2" && scheme != "Chameleon")
	{
		_granularity = config.get<uint32_t>("sys.mem.mcdram.cache_granularity");
		_num_ways = config.get<uint32_t>("sys.mem.mcdram.num_ways");
		_mcdram_type = config.get<const char *>("sys.mem.mcdram.type", "Simple");
		_cache_size = config.get<uint32_t>("sys.mem.mcdram.size", 128) * 1024 * 1024;
	}
	if (scheme == "AlloyCache")
	{
		_scheme = AlloyCache;
		assert(_granularity == 64);
		assert(_num_ways == 1);
	}
	else if (scheme == "UnisonCache")
	{
		assert(_granularity == 4096);
		_scheme = UnisonCache;
		_footprint_size = config.get<uint32_t>("sys.mem.mcdram.footprint_size");
	}
	else if (scheme == "HMA")
	{
		assert(_granularity == 4096);
		assert(_num_ways == _cache_size / _granularity);
		_scheme = HMA;
	}
	else if (scheme == "HybridCache")
	{
		// 4KB page or 2MB page
		assert(_granularity == 4096 || _granularity == 4096 * 512);
		_scheme = HybridCache;
	}
	else if (scheme == "NoCache")
		_scheme = NoCache;
	else if (scheme == "CacheOnly")
		_scheme = CacheOnly;
	else if (scheme == "Tagless")
	{
		_scheme = Tagless;
		_next_evict_idx = 0;
		_footprint_size = config.get<uint32_t>("sys.mem.mcdram.footprint_size");
	}
	else if (scheme == "Hybrid2")
	{ // 【newAddition】新增hybird2模式接口
		_scheme = Hybrid2;
	}
	else if (scheme == "Chameleon")
	{
		_scheme = Chameleon;
	}
	else
	{
		printf("scheme=%s\n", scheme.c_str());
		assert(false);
	}

	g_string placement_scheme = config.get<const char *>("sys.mem.mcdram.placementPolicy", "LRU");
	_bw_balance = config.get<bool>("sys.mem.bwBalance", false);
	_ds_index = 0;
	if (_bw_balance)
		assert(_scheme == AlloyCache || _scheme == HybridCache);

	// Configure the external Dram
	g_string ext_dram_name = _name + g_string("-ext");
	if (_ext_type == "Simple")
	{
		uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
		_ext_dram = (SimpleMemory *)gm_malloc(sizeof(SimpleMemory));
		new (_ext_dram) SimpleMemory(latency, ext_dram_name, config);
	}
	else if (_ext_type == "DDR")
		_ext_dram = BuildDDRMemory(config, frequency, domain, ext_dram_name, "sys.mem.ext_dram.", 4, 1.0);
	else if (_ext_type == "MD1")
	{
		uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
		uint32_t bandwidth = config.get<uint32_t>("sys.mem.ext_dram.bandwidth", 6400);
		_ext_dram = (MD1Memory *)gm_malloc(sizeof(MD1Memory));
		new (_ext_dram) MD1Memory(64, frequency, bandwidth, latency, ext_dram_name);
	}
	else if (_ext_type == "DRAMSim")
	{
		uint64_t cpuFreqHz = 1000000 * frequency;
		uint32_t capacity = config.get<uint32_t>("sys.mem.capacityMB", 16384);
		string dramTechIni = config.get<const char *>("sys.mem.techIni");
		string dramSystemIni = config.get<const char *>("sys.mem.systemIni");
		string outputDir = config.get<const char *>("sys.mem.outputDir");
		string traceName = config.get<const char *>("sys.mem.traceName", "dramsim");
		traceName += "_ext";
		_ext_dram = (DRAMSimMemory *)gm_malloc(sizeof(DRAMSimMemory));
		uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
		new (_ext_dram) DRAMSimMemory(dramTechIni, dramSystemIni, outputDir, traceName, capacity, cpuFreqHz, latency, domain, name);
	}
	else
		panic("Invalid memory controller type %s", _ext_type.c_str());

	if (_scheme != NoCache && _scheme != Hybrid2 && _scheme != Chameleon)
	{
		// Configure the MC-Dram (Timing Model)
		_mcdram_per_mc = config.get<uint32_t>("sys.mem.mcdram.mcdramPerMC", 4);
		//_mcdram = new MemObject * [_mcdram_per_mc];
		_mcdram = (MemObject **)gm_malloc(sizeof(MemObject *) * _mcdram_per_mc);
		for (uint32_t i = 0; i < _mcdram_per_mc; i++)
		{
			g_string mcdram_name = _name + g_string("-mc-") + g_string(to_string(i).c_str());
			// g_string mcdram_name(ss.str().c_str());
			if (_mcdram_type == "Simple")
			{
				uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
				_mcdram[i] = (SimpleMemory *)gm_malloc(sizeof(SimpleMemory));
				new (_mcdram[i]) SimpleMemory(latency, mcdram_name, config);
				//_mcdram[i] = new SimpleMemory(latency, mcdram_name, config);
			}
			else if (_mcdram_type == "DDR")
			{
				// XXX HACK tBL for mcdram is 1, so for data access, should multiply by 2, for tad access, should multiply by 3.
				_mcdram[i] = BuildDDRMemory(config, frequency, domain, mcdram_name, "sys.mem.mcdram.", 1, timing_scale);
			}
			else if (_mcdram_type == "MD1")
			{
				uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
				uint32_t bandwidth = config.get<uint32_t>("sys.mem.mcdram.bandwidth", 12800);
				_mcdram[i] = (MD1Memory *)gm_malloc(sizeof(MD1Memory));
				new (_mcdram[i]) MD1Memory(64, frequency, bandwidth, latency, mcdram_name);
			}
			else if (_mcdram_type == "DRAMSim")
			{
				uint64_t cpuFreqHz = 1000000 * frequency;
				uint32_t capacity = config.get<uint32_t>("sys.mem.capacityMB", 16384);
				string dramTechIni = config.get<const char *>("sys.mem.techIni");
				string dramSystemIni = config.get<const char *>("sys.mem.systemIni");
				string outputDir = config.get<const char *>("sys.mem.outputDir");
				string traceName = config.get<const char *>("sys.mem.traceName");
				traceName += "_mc";
				traceName += to_string(i);
				_mcdram[i] = (DRAMSimMemory *)gm_malloc(sizeof(DRAMSimMemory));
				uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
				new (_mcdram[i]) DRAMSimMemory(dramTechIni, dramSystemIni, outputDir, traceName, capacity, cpuFreqHz, latency, domain, name);
			}
			else
				panic("Invalid memory controller type %s", _mcdram_type.c_str());
		}
		// Configure MC-Dram Functional Model
		_num_sets = _cache_size / _num_ways / _granularity;
		if (_scheme == Tagless)
			assert(_num_sets == 1);
		_cache = (Set *)gm_malloc(sizeof(Set) * _num_sets);
		for (uint64_t i = 0; i < _num_sets; i++)
		{
			_cache[i].ways = (Way *)gm_malloc(sizeof(Way) * _num_ways);
			_cache[i].num_ways = _num_ways;
			for (uint32_t j = 0; j < _num_ways; j++)
				_cache[i].ways[j].valid = false;
		}
		if (_scheme == AlloyCache)
		{
			_line_placement_policy = (LinePlacementPolicy *)gm_malloc(sizeof(LinePlacementPolicy));
			new (_line_placement_policy) LinePlacementPolicy();
			_line_placement_policy->initialize(config);
		}
		else if (_scheme == HMA)
		{
			_os_placement_policy = (OSPlacementPolicy *)gm_malloc(sizeof(OSPlacementPolicy));
			new (_os_placement_policy) OSPlacementPolicy(this);
		}
		else if (_scheme == UnisonCache || _scheme == HybridCache)
		{
			_page_placement_policy = (PagePlacementPolicy *)gm_malloc(sizeof(PagePlacementPolicy));
			new (_page_placement_policy) PagePlacementPolicy(this);
			_page_placement_policy->initialize(config);
		}
	}
	if (_scheme == HybridCache)
	{
		_tag_buffer = (TagBuffer *)gm_malloc(sizeof(TagBuffer));
		new (_tag_buffer) TagBuffer(config);
	}
	if (_scheme == Hybrid2)
	{ // 【newAddition】 新增Hybrid2。此处代码接收2种类型的参数
		// 这样的设计就只有通道没有伪通道的概念
		// HBM通道数设置，按照道理来说应该是需要保持一致的
		_cache_hbm_per_mc = config.get<uint32_t>("sys.mem.cachehbm.cacheHBMPerMC", 4);
		_mem_hbm_per_mc = config.get<uint32_t>("sys.mem.memhbm.memHBMPerMC", 4);
		// 用作cache的HBM和用作memory的HBM设置
		// _cachehbm = (MemObject **) gm_malloc(sizeof(MemObject *) * _cache_hbm_per_mc);
		// _memhbm = (MemObject **) gm_malloc(sizeof(MemObject *) * _mem_hbm_per_mc);
		// 把大小也传进来,主要是传进来memhbm大小，这样可以根据lineAddr判断在哪一个内存介质
		_cache_hbm_size = config.get<uint32_t>("sys.mem.cachehbm.size", 64) * 1024 * 1024; // Default:64MB
		_mem_hbm_size = config.get<uint32_t>("sys.mem.memhbm.size", 1024) * 1024 * 1024;   // Default:1GB
		_cache_hbm_type = config.get<const char *>("sys.mem.cachehbm.type", "DDR");
		_mem_hbm_type = config.get<const char *>("sys.mem.memhbm.type", "DDR");

		// 所有_memhbm被替换为_mcdram 以避免未知错误
		// 请注意将_mcdram和_mchbm的配置文件进行统一，以避免不会暴露的bug
		_mcdram_per_mc = config.get<uint32_t>("sys.mem.mcdram.mcdramPerMC", 4);
		_mcdram = (MemObject **)gm_malloc(sizeof(MemObject *) * _mcdram_per_mc);
		for (uint32_t i = 0; i < _mcdram_per_mc; i++)
		{
			g_string mcdram_name = _name + g_string("-mc-") + g_string(to_string(i).c_str());
			_mcdram[i] = BuildDDRMemory(config, frequency, domain, mcdram_name, "sys.mem.mcdram.", 1, timing_scale);
		}

		// // 目前假定这里的hbm的type都是ddr类型,循环创建memHBM
		// for (uint32_t i = 0; i < _mem_hbm_per_mc ; i++){
		// 	g_string memhbm_name = _name + g_string("-memhbm-") + g_string(to_string(i).c_str());
		// 	_memhbm[i] = BuildDDRMemory(config, frequency, domain, memhbm_name, "sys.mem.memdram.", 4, timing_scale);

		// }

		// assert(_memhbm[0] != nullptr);
		// std::cout << "_memhbm[0] =========" << _memhbm[0] << std::endl;

		// 这里使用std::vector存储XTAEntry
		// XTAEntries的数量为set的数量
		// set的数量计算公式为_cache_hbm_size / (set_assoc_num * _hybrid2_page_size)
		// 即以page为粒度管理
		// 问题求解逻辑为 CacheHBM大小 一个set 可以映射 set_assoc_num 个 page-size大小的page
		// 问你需要多少个set
		_hybrid2_page_size = config.get<uint32_t>("sys.mem.pagesize", 4) * 1024; // in Bytes
		_hybrid2_blk_size = config.get<uint32_t>("sys.mem.blksize", 64);		 // in Bytes
		assert(_hybrid2_blk_size != 0);
		hybrid2_blk_per_page = _hybrid2_page_size / _hybrid2_blk_size;		// Default = 64
		set_assoc_num = config.get<uint32_t>("sys.mem.cachehbm.setnum", 8); // Default:8
		assert(set_assoc_num * _hybrid2_page_size != 0);
		hbm_set_num = _cache_hbm_size / (set_assoc_num * _hybrid2_page_size);
		hbm_pages_per_set = _mem_hbm_size / _hybrid2_page_size / set_assoc_num;
		// hybrid2_blk_per_page = _hybrid2_page_size / _hybrid2_blk_size;

		// 推荐不在config里修改，在这里修改即可
		phy_mem_size = config.get<uint64_t>("sys.mem.totalSize",9)*1024*1024*1024;
		
		num_pages = phy_mem_size / _hybrid2_page_size;
		for (uint64_t vpgnum = 0; vpgnum < num_pages; ++vpgnum)
		{
			fixedMapping[vpgnum] = vpgnum % num_pages;
		}

		// 循环创建XTAEntry
		assert(hbm_set_num > 0);
		for (uint64_t i = 0; i < hbm_set_num; ++i)
		{
			// 初始化一个set对应的XTAEntries,共有hbm_set_num个
			std::vector<XTAEntry> entries;

			// 循环初始化XTAEntry,一个set固定set_assoc_num个page
			for (uint64_t j = 0; j < set_assoc_num; j++)
			{
				XTAEntry tmp_entry;
				tmp_entry._hybrid2_tag = 0;
				tmp_entry._hbm_tag = 0;
				tmp_entry._dram_tag = 0;
				tmp_entry._hybrid2_LRU = 0; // LRU的逻辑应该有其它的设置方式，目前暂时先不考虑
				tmp_entry._hybrid2_counter = 0;
				// 还剩下2个vector需要设置，先对指针数组初始化
				// tmp_entry.bit_vector = (uint64_t*)malloc(hybrid2_blk_per_page * sizeof(uint64_t));
				// tmp_entry.dirty_vector = (uint64_t*)malloc(hybrid2_blk_per_page * sizeof(uint64_t));
				// tmp_entry.bit_vector = new uint64_t[hybrid2_blk_per_page];
				// tmp_entry.dirty_vector = new uint64_t[hybrid2_blk_per_page];
				for (uint64_t k = 0; k < (uint64_t)hybrid2_blk_per_page; k++)
				{
					tmp_entry.bit_vector[k] = 0;
					tmp_entry.dirty_vector[k] = 0;
				}
				// 假设组相联数为8 这里会将8个页面对应的XTAEntry加入XTAEntries(SetEntries更准确一点)
				entries.push_back(tmp_entry);
			}
			// SetEntries加入XTA
			XTA.push_back(entries);
		}

		// 循环初始化DRAM HBM内存占用情况 [暂时不想用这个]
		// for(uint64_t i=0; i < hbm_set_num; i++)
		// {
		// 	std::vector<int> SETEntries_occupied;
		// 	memory_occupied.push_back(SETEntries_occupied);
		// 	// 按理来说是还有dram—_pages_per_set的 但是这里假设了DRAM空间无限大，怎么写需要考虑，TODO
		// 	// 暂时设置一个DRAM大小比例吧，后续可调
		// 	int x = 8;
		// 	for(uint64_t j = 0; j < (1+x)*hbm_pages_per_set ; j++)
		// 	{
		// 		memory_occupied[i].push_back(0);
		// 	}
		// }
	}

	if (_scheme == Chameleon){
		_mem_hbm_per_mc = config.get<uint32_t>("sys.mem.memhbm.memHBMPerMC", 4);
		_mem_hbm_size = config.get<uint32_t>("sys.mem.memhbm.size",1024)*1024*1024;// Default:1GB
		_mem_hbm_type = config.get<const char*>("sys.mem.memhbm.type","DDR");
		_mcdram_per_mc = config.get<uint32_t>("sys.mem.mcdram.mcdramPerMC", 4);
		_mcdram = (MemObject **) gm_malloc(sizeof(MemObject *) * _mcdram_per_mc);
		for (uint32_t i = 0; i < _mcdram_per_mc; i++)
		{
			g_string mcdram_name = _name + g_string("-mc-") + g_string(to_string(i).c_str());
			_mcdram[i] = BuildDDRMemory(config, frequency, domain, mcdram_name, "sys.mem.mcdram.", 1, timing_scale);
		}
		phy_mem_size = config.get<uint32_t>("sys.mem.totalSize",9)*1024*1024*1024;
		_chameleon_blk_size=config.get<uint64_t>("sys.mem.mcdram.blksize",64);

		int ddrRatio = (int)(phy_mem_size - _mem_hbm_size) / _mem_hbm_size;
		// 要多少segment
		// 估算元数据开销：segGrpEntry一个接近4B 1GB/64B*4B = 64MB
		uint32_t _segment_number =	_mem_hbm_size / _chameleon_blk_size;
		// 初始化
		for(uint32_t i = 0; i<_segment_number ; i++)
		{
			segGrpEntry tmpEntry(ddrRatio);
			segGrps.push_back(tmpEntry);
		}

	}
	// Stats
	_num_hit_per_step = 0;
	_num_miss_per_step = 0;
	_mc_bw_per_step = 0;
	_ext_bw_per_step = 0;
	for (uint32_t i = 0; i < MAX_STEPS; i++)
		_miss_rate_trace[i] = 0;
	_num_requests = 0;
}

uint64_t
MemoryController::access(MemReq &req)
{
	// std::cout << std::hex << "In access , Primary Req vLineAddr:   0x" << req.lineAddr << std::endl;
	switch (req.type)
	{
	case PUTS:
	case PUTX:
		*req.state = I;
		break;
	case GETS:
		*req.state = req.is(MemReq::NOEXCL) ? S : E;
		break;
	case GETX:
		*req.state = M;
		break;
	default:
		panic("!?");
	}
	if (req.type == PUTS)
		return req.cycle;
	futex_lock(&_lock);
	// ignore clean LLC eviction
	if (_collect_trace && _name == "mem-0")
	{
		_address_trace[_cur_trace_len] = req.lineAddr;
		_cycle_trace[_cur_trace_len] = req.cycle;
		_type_trace[_cur_trace_len] = (req.type == PUTX) ? 1 : 0;
		_cur_trace_len++;
		assert(_cur_trace_len <= _max_trace_len);
		if (_cur_trace_len == _max_trace_len)
		{
			// FILE * f = fopen((_trace_dir + g_string("/") + _name + g_string("trace.bin")).c_str(), "ab");
			FILE *f = fopen((_trace_dir + g_string("/") + _name + g_string("trace.txt")).c_str(), "a"); // 使用 "a" 以追加模式打开文件
			for (size_t i = 0; i < _max_trace_len; i++)
			{
				fprintf(f, "%lu, %lx, %u\n", _cycle_trace[i], _address_trace[i], _type_trace[i]);
			}
			// fwrite(_address_trace, sizeof(Address), _max_trace_len, f);
			// fwrite(_type_trace, sizeof(uint32_t), _max_trace_len, f);
			fclose(f);
			_cur_trace_len = 0;
		}
	}

	_num_requests++;
	if (_scheme == NoCache)
	{
		///////   load from external dram
		req.cycle = _ext_dram->access(req, 0, 4);
		_numLoadHit.inc();
		futex_unlock(&_lock);
		return req.cycle;
		////////////////////////////////////
	}

	if (_scheme == Hybrid2)
	{
		// 请勿在同一个调用链里调用处理req的请求两次！
		// std::cout << "Still ret req.cycle  00000  = " << hbm_hybrid2_access(req) <<std::endl;
		req.cycle = hybrid2_access(req);
		// std::cout << "Still ret req.cycle    11111 = " << req.cycle <<std::endl;
		// futex_unlock(&_lock); // 此处重复释放锁，涉及到锁的一致性问题。
		return req.cycle;
	}

	// if(_scheme==Chameleon)
	// {
	// 	// 请勿在同一个调用链里调用处理req的请求两次！
	// 	// std::cout << "Still ret req.cycle  00000  = " << hbm_hybrid2_access(req) <<std::endl;
	// 	req.cycle = chameleon_access(req);
	// 	// std::cout << "Still ret req.cycle    11111 = " << req.cycle <<std::endl;
	// 	// futex_unlock(&_lock); // 此处重复释放锁，涉及到锁的一致性问题。
	// 	return req.cycle;
	// }

	/////////////////////////////
	// TODO For UnisonCache
	// should correctly model way accesses
	/////////////////////////////


	ReqType type = (req.type == GETS || req.type == GETX) ? LOAD : STORE;
	Address address = req.lineAddr;
	uint32_t mcdram_select = (address / 64) % _mcdram_per_mc;
	Address mc_address = (address / 64 / _mcdram_per_mc * 64) | (address % 64);
	// printf("address=%ld, _mcdram_per_mc=%d, mc_address=%ld\n", address, _mcdram_per_mc, mc_address);
	Address tag = address / (_granularity / 64);
	uint64_t set_num = tag % _num_sets;
	uint32_t hit_way = _num_ways;
	// uint64_t orig_cycle = req.cycle;
	uint64_t data_ready_cycle = req.cycle;
	MESIState state;

	if (_scheme == CacheOnly)
	{
		///////   load from mcdram
		// std::cout << "Channel Select = " << mcdram_select << "  |||  CacheOnly req.lineAddr = " << req.lineAddr << std::endl;
		req.lineAddr = mc_address;
		req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
		req.lineAddr = address;
		_numLoadHit.inc();
		futex_unlock(&_lock);
		return req.cycle;
		////////////////////////////////////
	}
	uint64_t step_length = _cache_size / 64 / 10;

	// whether needs to probe tag for HybridCache.
	// need to do so for LLC dirty eviction and if the page is not in TB
	bool hybrid_tag_probe = false;
	if (_granularity >= 4096)
	{
		if (_tlb.find(tag) == _tlb.end())
			_tlb[tag] = TLBEntry{tag, _num_ways, 0, 0, 0};
		if (_tlb[tag].way != _num_ways)
		{
			hit_way = _tlb[tag].way;
			assert(_cache[set_num].ways[hit_way].valid && _cache[set_num].ways[hit_way].tag == tag);
		}
		else if (_scheme != Tagless)
		{
			// for Tagless, this assertion takes too much time.
			for (uint32_t i = 0; i < _num_ways; i++)
				assert(_cache[set_num].ways[i].tag != tag || !_cache[set_num].ways[i].valid);
		}

		if (_scheme == UnisonCache)
		{
			//// Tag and data access. For simplicity, use a single access.
			if (type == LOAD)
			{
				req.lineAddr = mc_address; // transMCAddressPage(set_num, 0); //mc_address;
				req.cycle = _mcdram[mcdram_select]->access(req, 0, 6);
				_mc_bw_per_step += 6;
				_numTagLoad.inc();
				req.lineAddr = address;
			}
			else
			{
				assert(type == STORE);
				MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);
				_mc_bw_per_step += 2;
				_numTagLoad.inc();
			}
			///////////////////////////////
		}
		if (_scheme == HybridCache && type == STORE)
		{
			if (_tag_buffer->existInTB(tag) == _tag_buffer->getNumWays() && set_num >= _ds_index)
			{
				_numTBDirtyMiss.inc();
				if (!_sram_tag)
					hybrid_tag_probe = true;
			}
			else
				_numTBDirtyHit.inc();
		}
		if (_scheme == HybridCache && _sram_tag)
			req.cycle += _llc_latency;
	}
	else if (_scheme == AlloyCache)
	{ // 这里从else 变为 else if
		// assert(_scheme == AlloyCache);
		if (_cache[set_num].ways[0].valid && _cache[set_num].ways[0].tag == tag && set_num >= _ds_index)
			hit_way = 0;
		if (type == LOAD && set_num >= _ds_index)
		{
			///// mcdram TAD access
			// Modeling TAD as 2 cachelines
			if (_sram_tag)
			{
				req.cycle += _llc_latency;
				/*				if (hit_way == 0) {
									req.lineAddr = mc_address;
									req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
									_mc_bw_per_step += 4;
									_numTagLoad.inc();
									req.lineAddr = address;
								}
				*/
			}
			else
			{
				req.lineAddr = mc_address;
				req.cycle = _mcdram[mcdram_select]->access(req, 0, 6);
				_mc_bw_per_step += 6;
				_numTagLoad.inc();
				req.lineAddr = address;
			}
			///////////////////////////////
		}
	}
	else
	{
		// 这里目前是只能是hybrid2,暂时先assert
		// 按理来说这段代码应该解耦出去，但是不知道有哪些地方调用了这个access，改动代价较高
		// 但是也可以尝试
		// 解锁操作移动到hybrid2_access
		// assert(_scheme == Hybrid2);
		// // 重新写一个访问函数
		// return hybrid2_access(req);
	}
	bool cache_hit = hit_way != _num_ways;

	// orig_cycle = req.cycle;
	//  dram cache logic. Here, I'm assuming the 4 mcdram channels are
	//  organized centrally
	bool counter_access = false;
	// use the following state for requests, so that req.state is not changed
	if (!cache_hit)
	{
		uint64_t cur_cycle = req.cycle;
		_num_miss_per_step++;
		if (type == LOAD)
			_numLoadMiss.inc();
		else
			_numStoreMiss.inc();

		uint32_t replace_way = _num_ways;
		if (_scheme == AlloyCache)
		{
			bool place = false;
			if (set_num >= _ds_index)
				place = _line_placement_policy->handleCacheMiss(&_cache[set_num].ways[0]);
			replace_way = place ? 0 : 1;
		}
		else if (_scheme == HMA)
			_os_placement_policy->handleCacheAccess(tag, type);
		else if (_scheme == Tagless)
		{
			replace_way = _next_evict_idx;
			_next_evict_idx = (_next_evict_idx + 1) % _num_ways;
		}
		else
		{
			if (set_num >= _ds_index)
				replace_way = _page_placement_policy->handleCacheMiss(tag, type, set_num, &_cache[set_num], counter_access);
		}

		/////// load from external dram
		if (_scheme == AlloyCache)
		{
			if (type == LOAD)
			{
				if (!_sram_tag && set_num >= _ds_index)
					req.cycle = _ext_dram->access(req, 1, 4);
				else
					req.cycle = _ext_dram->access(req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
			else if (type == STORE && replace_way >= _num_ways)
			{
				// no replacement
				req.cycle = _ext_dram->access(req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
			else if (type == STORE)
			{ // && replace_way < _num_ways)
				MemReq load_req = {address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _ext_dram->access(load_req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
		}
		else if (_scheme == HMA)
		{
			req.cycle = _ext_dram->access(req, 0, 4);
			_ext_bw_per_step += 4;
			data_ready_cycle = req.cycle;
		}
		else if (_scheme == UnisonCache)
		{
			if (type == LOAD)
			{
				req.cycle = _ext_dram->access(req, 1, 4);
				_ext_bw_per_step += 4;
			}
			else if (type == STORE && replace_way >= _num_ways)
			{
				req.cycle = _ext_dram->access(req, 1, 4);
				_ext_bw_per_step += 4;
			}
			data_ready_cycle = req.cycle;
		}
		else if (_scheme == HybridCache)
		{
			if (hybrid_tag_probe)
			{
				MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);
				_mc_bw_per_step += 2;
				req.cycle = _ext_dram->access(req, 1, 4);
				_ext_bw_per_step += 4;
				_numTagLoad.inc();
				data_ready_cycle = req.cycle;
			}
			else
			{
				req.cycle = _ext_dram->access(req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
		}
		else if (_scheme == Tagless)
		{
			assert(_ext_dram);
			req.cycle = _ext_dram->access(req, 0, 4);
			_ext_bw_per_step += 4;
			data_ready_cycle = req.cycle;
		}
		////////////////////////////////////

		if (replace_way < _num_ways)
		{
			///// mcdram replacement
			// TODO update the address
			if (_scheme == AlloyCache)
			{
				MemReq insert_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				uint32_t size = _sram_tag ? 4 : 6;
				_mcdram[mcdram_select]->access(insert_req, 2, size);
				_mc_bw_per_step += size;
				_numTagStore.inc();
			}
			else if (_scheme == UnisonCache || _scheme == HybridCache || _scheme == Tagless)
			{
				uint32_t access_size = (_scheme == UnisonCache || _scheme == Tagless) ? _footprint_size : (_granularity / 64);
				// load page from ext dram
				MemReq load_req = {tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				_ext_dram->access(load_req, 2, access_size * 4);
				_ext_bw_per_step += access_size * 4;
				// store the page to mcdram
				MemReq insert_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				_mcdram[mcdram_select]->access(insert_req, 2, access_size * 4);
				_mc_bw_per_step += access_size * 4;
				if (_scheme == Tagless)
				{
					MemReq load_gipt_req = {tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
					MemReq store_gipt_req = {tag * 64, PUTS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
					_ext_dram->access(load_gipt_req, 2, 2);	 // update GIPT
					_ext_dram->access(store_gipt_req, 2, 2); // update GIPT
					_ext_bw_per_step += 4;
				}
				else if (!_sram_tag)
				{
					_mcdram[mcdram_select]->access(insert_req, 2, 2); // store tag
					_mc_bw_per_step += 2;
				}
				_numTagStore.inc();
			}

			///////////////////////////////
			_numPlacement.inc();
			if (_cache[set_num].ways[replace_way].valid)
			{
				Address replaced_tag = _cache[set_num].ways[replace_way].tag;
				// Note that tag_buffer is not updated if placed into an invalid entry.
				// this is like ignoring the initialization cost
				if (_scheme == HybridCache)
				{
					// Update TagBuffer
					// if (!_tag_buffer->canInsert(tag, replaced_tag)) {
					//	printf("!!!!!!Occupancy = %f\n", _tag_buffer->getOccupancy());
					//	_tag_buffer->clearTagBuffer();
					//	_numTagBufferFlush.inc();
					//}
					// assert (_tag_buffer->canInsert(tag, replaced_tag));
					assert(_tag_buffer->canInsert(tag, replaced_tag));
					{
						_tag_buffer->insert(tag, true);
						_tag_buffer->insert(replaced_tag, true);
					}
					// else {
					//	goto end;
					// }
				}

				_tlb[replaced_tag].way = _num_ways;
				// only used for UnisonCache
				uint32_t unison_dirty_lines = __builtin_popcountll(_tlb[replaced_tag].dirty_bitvec) * 4;
				uint32_t unison_touch_lines = __builtin_popcountll(_tlb[replaced_tag].touch_bitvec) * 4;
				if (_scheme == UnisonCache || _scheme == Tagless)
				{
					assert(unison_touch_lines > 0);
					assert(unison_touch_lines <= 64);
					assert(unison_dirty_lines <= 64);
					_numTouchedLines.inc(unison_touch_lines);
					_numEvictedLines.inc(unison_dirty_lines);
				}

				if (_cache[set_num].ways[replace_way].dirty)
				{
					_numDirtyEviction.inc();
					///////   store dirty line back to external dram
					// Store starts after TAD is loaded.
					// request not on critical path.
					if (_scheme == AlloyCache)
					{
						if (type == STORE)
						{
							if (_sram_tag)
							{
								MemReq load_req = {mc_address, GETS, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
								req.cycle = _mcdram[mcdram_select]->access(load_req, 2, 4);
								_mc_bw_per_step += 4;
								//_numTagLoad.inc();
							}
						}
						MemReq wb_req = {_cache[set_num].ways[replace_way].tag, PUTX, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(wb_req, 2, 4);
						_ext_bw_per_step += 4;
					}
					else if (_scheme == HybridCache)
					{
						// load page from mcdram
						MemReq load_req = {mc_address, GETS, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[mcdram_select]->access(load_req, 2, (_granularity / 64) * 4);
						_mc_bw_per_step += (_granularity / 64) * 4;
						// store page to ext dram
						// TODO. this event should be appended under the one above.
						// but they are parallel right now.
						MemReq wb_req = {_cache[set_num].ways[replace_way].tag * 64, PUTX, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(wb_req, 2, (_granularity / 64) * 4);
						_ext_bw_per_step += (_granularity / 64) * 4;
					}
					else if (_scheme == UnisonCache || _scheme == Tagless)
					{
						assert(unison_dirty_lines > 0);
						// load page from mcdram
						assert(unison_dirty_lines <= 64);
						MemReq load_req = {mc_address, GETS, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[mcdram_select]->access(load_req, 2, unison_dirty_lines * 4);
						_mc_bw_per_step += unison_dirty_lines * 4;
						// store page to ext dram
						// TODO. this event should be appended under the one above.
						// but they are parallel right now.
						MemReq wb_req = {_cache[set_num].ways[replace_way].tag * 64, PUTX, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(wb_req, 2, unison_dirty_lines * 4);
						_ext_bw_per_step += unison_dirty_lines * 4;
						if (_scheme == Tagless)
						{
							MemReq load_gipt_req = {tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							MemReq store_gipt_req = {tag * 64, PUTS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_ext_dram->access(load_gipt_req, 2, 2);	 // update GIPT
							_ext_dram->access(store_gipt_req, 2, 2); // update GIPT
							_ext_bw_per_step += 4;
						}
					}

					/////////////////////////////
				}
				else
				{
					_numCleanEviction.inc();
					if (_scheme == UnisonCache || _scheme == Tagless)
						assert(unison_dirty_lines == 0);
				}
			}
			_cache[set_num].ways[replace_way].valid = true;
			_cache[set_num].ways[replace_way].tag = tag;
			_cache[set_num].ways[replace_way].dirty = (req.type == PUTX);
			_tlb[tag].way = replace_way;
			if (_scheme == UnisonCache || _scheme == Tagless)
			{
				uint64_t bit = (address - tag * 64) / 4;
				assert(bit < 16 && bit >= 0);
				bit = ((uint64_t)1UL) << bit;
				_tlb[tag].touch_bitvec = 0;
				_tlb[tag].dirty_bitvec = 0;
				_tlb[tag].touch_bitvec |= bit;
				if (type == STORE)
					_tlb[tag].dirty_bitvec |= bit;
			}
		}
		else
		{
			// Miss but no replacement
			if (_scheme == HybridCache)
				if (type == LOAD && _tag_buffer->canInsert(tag))
					_tag_buffer->insert(tag, false);
			assert(_scheme != Tagless)
		}
	}
	else
	{ // cache_hit == true
		assert(set_num >= _ds_index);
		if (_scheme == AlloyCache)
		{
			if (type == LOAD && _sram_tag)
			{
				MemReq read_req = {mc_address, GETX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(read_req, 0, 4);
				_mc_bw_per_step += 4;
			}
			if (type == STORE)
			{
				// LLC dirty eviction hit
				MemReq write_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(write_req, 0, 4);
				_mc_bw_per_step += 4;
			}
		}
		else if (_scheme == UnisonCache && type == STORE)
		{
			// LLC dirty eviction hit
			MemReq write_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
			req.cycle = _mcdram[mcdram_select]->access(write_req, 1, 4);
			_mc_bw_per_step += 4;
		}
		if (_scheme == AlloyCache || _scheme == UnisonCache)
			data_ready_cycle = req.cycle;
		_num_hit_per_step++;
		if (_scheme == HMA)
			_os_placement_policy->handleCacheAccess(tag, type);
		else if (_scheme == HybridCache || _scheme == UnisonCache)
		{
			_page_placement_policy->handleCacheHit(tag, type, set_num, &_cache[set_num], counter_access, hit_way);
		}

		if (req.type == PUTX)
		{
			_numStoreHit.inc();
			_cache[set_num].ways[hit_way].dirty = true;
		}
		else
			_numLoadHit.inc();

		if (_scheme == HybridCache)
		{
			if (!hybrid_tag_probe)
			{
				req.lineAddr = mc_address;
				req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
				_mc_bw_per_step += 4;
				req.lineAddr = address;
				data_ready_cycle = req.cycle;
				if (type == LOAD && _tag_buffer->canInsert(tag))
					_tag_buffer->insert(tag, false);
			}
			else
			{
				assert(!_sram_tag);
				MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);
				_mc_bw_per_step += 2;
				_numTagLoad.inc();
				req.lineAddr = mc_address;
				req.cycle = _mcdram[mcdram_select]->access(req, 1, 4);
				_mc_bw_per_step += 4;
				req.lineAddr = address;
				data_ready_cycle = req.cycle;
			}
		}
		else if (_scheme == Tagless)
		{
			req.lineAddr = mc_address;
			req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
			_mc_bw_per_step += 4;
			req.lineAddr = address;
			data_ready_cycle = req.cycle;

			uint64_t bit = (address - tag * 64) / 4;
			assert(bit < 16 && bit >= 0);
			bit = ((uint64_t)1UL) << bit;
			_tlb[tag].touch_bitvec |= bit;
			if (type == STORE)
				_tlb[tag].dirty_bitvec |= bit;
		}

		//// data access
		if (_scheme == HMA)
		{
			req.lineAddr = mc_address; // transMCAddressPage(set_num, hit_way); //mc_address;
			req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
			_mc_bw_per_step += 4;
			req.lineAddr = address;
			data_ready_cycle = req.cycle;
		}
		if (_scheme == UnisonCache)
		{
			// Update LRU information for UnisonCache
			MemReq tag_update_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
			_mcdram[mcdram_select]->access(tag_update_req, 2, 2);
			_mc_bw_per_step += 2;
			_numTagStore.inc();
			uint64_t bit = (address - tag * 64) / 4;
			assert(bit < 16 && bit >= 0);
			bit = ((uint64_t)1UL) << bit;
			_tlb[tag].touch_bitvec |= bit;
			if (type == STORE)
				_tlb[tag].dirty_bitvec |= bit;
		}
		///////////////////////////////
	}
	// end:
	//  TODO. make this part work again.
	if (counter_access && !_sram_tag)
	{
		// TODO may not need the counter load if we can store freq info inside TAD
		/////// model counter access in mcdram
		// One counter read and one coutner write
		assert(set_num >= _ds_index);
		_numCounterAccess.inc();
		MemReq counter_req = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
		_mcdram[mcdram_select]->access(counter_req, 2, 2);
		counter_req.type = PUTX;
		_mcdram[mcdram_select]->access(counter_req, 2, 2);
		_mc_bw_per_step += 4;
		//////////////////////////////////////
	}
	if (_scheme == HybridCache && _tag_buffer->getOccupancy() > 0.7)
	{
		printf("[Tag Buffer FLUSH] occupancy = %f\n", _tag_buffer->getOccupancy());
		_tag_buffer->clearTagBuffer();
		_tag_buffer->setClearTime(req.cycle);
		_numTagBufferFlush.inc();
	}

	// TODO. Make the timing info here correct.
	// TODO. should model system level stall
	if (_scheme == HMA && _num_requests % _os_quantum == 0)
	{
		uint64_t num_replace = _os_placement_policy->remapPages();
		_numPlacement.inc(num_replace * 2);
	}

	if (_num_requests % step_length == 0)
	{
		_num_hit_per_step /= 2;
		_num_miss_per_step /= 2;
		_mc_bw_per_step /= 2;
		_ext_bw_per_step /= 2;
		if (_bw_balance && _mc_bw_per_step + _ext_bw_per_step > 0)
		{
			// adjust _ds_index	based on mc vs. ext dram bandwidth.
			double ratio = 1.0 * _mc_bw_per_step / (_mc_bw_per_step + _ext_bw_per_step);
			double target_ratio = 0.8; // because mc_bw = 4 * ext_bw

			// the larger the gap between ratios, the more _ds_index changes.
			// _ds_index changes in the granualrity of 1/1000 dram cache capacity.
			// 1% in the ratio difference leads to 1/1000 _ds_index change.
			// 300 is arbitrarily chosen.
			// XXX XXX XXX
			// 1000 is only used for graph500 and pagerank.
			// uint64_t index_step = _num_sets / 300; // in terms of the number of sets
			uint64_t index_step = _num_sets / 1000; // in terms of the number of sets
			int64_t delta_index = (ratio - target_ratio > -0.02 && ratio - target_ratio < 0.02) ? 0 : index_step * (ratio - target_ratio) / 0.01;
			printf("ratio = %f\n", ratio);
			if (delta_index > 0)
			{
				// _ds_index will increase. All dirty data between _ds_index and _ds_index + delta_index
				// should be written back to external dram.
				// For Alloy cache, this is relatively easy.
				// For Hybrid, we need to update tag buffer as well...
				for (uint32_t mc = 0; mc < _mcdram_per_mc; mc++)
				{
					for (uint64_t set = _ds_index; set < (uint64_t)(_ds_index + delta_index); set++)
					{
						if (set >= _num_sets)
							break;
						for (uint32_t way = 0; way < _num_ways; way++)
						{
							Way &meta = _cache[set].ways[way];
							if (meta.valid && meta.dirty)
							{
								// should write back to external dram.
								MemReq load_req = {meta.tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_mcdram[mc]->access(load_req, 2, (_granularity / 64) * 4);
								MemReq wb_req = {meta.tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_ext_dram->access(wb_req, 2, (_granularity / 64) * 4);
								_ext_bw_per_step += (_granularity / 64) * 4;
								_mc_bw_per_step += (_granularity / 64) * 4;
							}
							if (_scheme == HybridCache && meta.valid)
							{
								_tlb[meta.tag].way = _num_ways;
								// for Hybrid cache, should insert to tag buffer as well.
								if (!_tag_buffer->canInsert(meta.tag))
								{
									printf("Rebalance. [Tag Buffer FLUSH] occupancy = %f\n", _tag_buffer->getOccupancy());
									_tag_buffer->clearTagBuffer();
									_tag_buffer->setClearTime(req.cycle);
									_numTagBufferFlush.inc();
								}
								assert(_tag_buffer->canInsert(meta.tag));
								_tag_buffer->insert(meta.tag, true);
							}
							meta.valid = false;
							meta.dirty = false;
						}
						if (_scheme == HybridCache)
							_page_placement_policy->flushChunk(set);
					}
				}
			}
			_ds_index = ((int64_t)_ds_index + delta_index <= 0) ? 0 : _ds_index + delta_index;
			printf("_ds_index = %ld/%ld\n", _ds_index, _num_sets);
		}
	}
	futex_unlock(&_lock);
	// uint64_t latency = req.cycle - orig_cycle;
	// req.cycle = orig_cycle;
	return data_ready_cycle; // req.cycle + latency;
}

/**
 * Todo List:
 * 1. Specific Memory Access Latency [Need to Clarify ReqType]
 * 2. req's parameters should be considered later.
 */
uint64_t
MemoryController::hybrid2_access(MemReq &req)
{
	assert(_scheme == Hybrid2);
	// futex_lock(&_lock);
	// std::cout << std::hex << "vaddr:   0x" << req.lineAddr << std::endl;
	Address tmpAddr = req.lineAddr;
	req.lineAddr = vaddr_to_paddr(req);
	// std::cout << std::hex << "vaddr to paddr:   0x" <<  req.lineAddr << std::endl;
	switch (req.type)
	{
	case PUTS:
	case PUTX:
		*req.state = I;
		break;
	case GETS:
		*req.state = req.is(MemReq::NOEXCL) ? S : E;
		break;
	case GETX:
		*req.state = M;
		break;
	default:
		panic("!?");
	}
	// 干净数据，不会改变块的状态
	if (req.type == PUTS)
	{
		return req.cycle;
	}
	// futex_unlock(&_lock); 这里的执行已经持有了这把锁，由于在access中直接返回
	// 在本回合（快要）结束（的某个时机），需要释放这把锁

	// 请求状态
	ReqType type = (req.type == GETS || req.type == GETX) ? LOAD : STORE;
	// 表示的地址
	Address address = req.lineAddr;
	// 地址按照64B Cacheline 对齐
	address = address / 64 * 64;
	// HBM在这里需要自己考虑分到哪一个通道，但是ZSim不太涉及请求排队

	// uint32_t cache_hbm_select = (address / 64) % _cache_hbm_per_mc;
	// Address cache_hbm_address = (address / 64 /_cache_hbm_per_mc * 64) | (address % 64);
	assert(0 != _cache_hbm_per_mc);
	uint32_t mem_hbm_select = (address / 64) % _cache_hbm_per_mc;
	// assert(4 > mem_hbm_select);
	Address mem_hbm_address = (address / 64 / _cache_hbm_per_mc * 64) | (address % 64);

	// address在哪一个page，在page第几个block
	// 保证内存对齐
	assert(0 != _hybrid2_blk_size);
	// uint64_t page_addr = (address / _hybrid2_page_size) * _hybrid2_page_size;
	uint64_t page_addr = get_page_id(address);
	// uint64_t blk_offset = (address - page_addr*_hybrid2_page_size) / _hybrid2_blk_size;
	// 先计算页内偏移再按block对齐
	// uint64_t blk_addr = ((address % _hybrid2_page_size) / _hybrid2_blk_size) * _hybrid2_blk_size;
	uint64_t blk_offset = (address % _hybrid2_page_size) / _hybrid2_blk_size;
	// std::cout << "blk_offset ==" << blk_offset << std::endl;

	// 根据程序的执行流，先访问XTA
	// 根据XTA的两层结构，应该先找到set，再找到Page
	// 所以需要先封装一个获取set的函数以降低耦合度
	uint64_t set_id = get_set_id(address);
	g_vector<XTAEntry> &SETEntries = find_XTA_set(set_id);
	// 遍历 这个SET
	bool if_XTA_hit = false;
	// bool is_dram = address >= _mem_hbm_size;

	// 为HBMTable服务，在迁移或逐出阶段，对于可能在HBM或remap到DRAM的数据使用
	uint64_t avg_temp = 0;
	uint64_t low_temp = 100000;

	// uint64_t look_up_XTA_lantency = 0;
	uint64_t total_latency = 0;
	// TODO 接下来我就是在SETEntries里找，看看能不能找到那个page,找到了就是XTAHit，否则就是XTAMiss
	// 找的逻辑是根据地址去找，匹配_hybrid2_tag
	// std::cout << "workflow come here :(loop) 1111111 !" << std::endl;
	for (uint64_t i = 0; i < set_assoc_num; i++)
	{
		// std::cout << "SETEntries[i].bit_vector[blk_offset] (000)===" << SETEntries[i].bit_vector[blk_offset] << std::endl;
		// std::cout << "workflow come here :(loop idx) xxxx !" << std::endl;
		// std::cout << i << std::endl;
		if (SETEntries[i]._hybrid2_counter > 0)
		{
			low_temp = low_temp > SETEntries[i]._hybrid2_counter ? SETEntries[i]._hybrid2_counter : low_temp;
		}
		avg_temp += SETEntries[i]._hybrid2_counter;
		// 这一部分我觉得是hybrid2_tag匹配的是page的地址,存疑
		// std::cout << "page_addr: " << page_addr << "    SETEntry.tag:  "<<  SETEntries[i]._hybrid2_tag << std::endl;
		if (page_addr == SETEntries[i]._hybrid2_tag)
		{
			// std::cout << "SETEntries[i].bit_vector[blk_offset] (111)===" << SETEntries[i].bit_vector[blk_offset] << std::endl;
			// 表示 XTA Hit 了
			if_XTA_hit = true;
			// std::cout << "workflow come here :(XTA Hit)!" << std::endl;
			// XTA Hit 意味着 Page也hit了，page hit 但是cacheline 不一定hit
			// 首先把LRU的值先改了,本Page LRU置为0，其余计数器+1，这里可以解耦一个计算最小最大LRU的函数
			for (uint64_t j = 0; j < set_assoc_num; j++)
			{
				SETEntries[j]._hybrid2_LRU++;
			}
			SETEntries[i]._hybrid2_LRU = 0;
			SETEntries[i]._hybrid2_counter += 1;

			int exist = SETEntries[i].bit_vector[blk_offset]; // 0 代表cacheline miss 1 代表 cacheline hit
			if (exist)
			{
				// std::cout << "exist[" << exist << "]workflow come here :(Cacheline Hit)!" << std::endl;
				// 访问HBM,TODO
				if (type == STORE)
				{
					// 这一步实际上涉及到了内存交织,在这里访问cacheHBM还是memHBM没有区别
					req.lineAddr = mem_hbm_address;
					// 第三个参数怎么设置，没想好，TODO
					req.cycle = _mcdram[mem_hbm_select]->access(req, 0, 6);
					// req.lineAddr = address;
					req.lineAddr = tmpAddr;
					total_latency += req.cycle;
					futex_unlock(&_lock);
					return total_latency;
				}
				else if (type == LOAD)
				{
					// 这一步实际上涉及到了内存交织
					req.lineAddr = mem_hbm_address;
					// 第三个参数怎么设置，没想好，TODO
					req.cycle = _mcdram[mem_hbm_select]->access(req, 0, 4);
					// req.lineAddr = address;
					req.lineAddr = tmpAddr;
					total_latency += req.cycle;
					futex_unlock(&_lock);
					return total_latency;
				}
			}
			else
			{
				// std::cout << "exist[" << exist << "]workflow come here :(Cacheline Miss)!" << std::endl;
				// 这里也有两种情况。Case1:有可能在DRAM里；Case2：有可能在HBM里
				// 两种情况都有可能出现remap的情况

				// 有可能是dram,有可能remap到hbm
				if (address >= _mem_hbm_size)
				{
					// 检查DRAMTable有没有存映射
					auto it = DRAMTable.find(address);
					if (it == DRAMTable.end())
					{
						// assert(it != DRAMTable.end());
						// 访问DRAM,TODO
						req.cycle = _ext_dram->access(req, 0, 4);
						req.lineAddr = tmpAddr;
						total_latency += req.cycle;
						// std::cout << "workflow over here (XTA Hit ,is dram)!!" << std::endl;
						futex_unlock(&_lock);
						return total_latency;
					}
					else
					{
						// 访问HBM,TODO
						uint64_t dest_address = it->second;

						uint64_t dest_hbm_mc_address = (dest_address / 64 / _mem_hbm_per_mc * 64) | (dest_address % 64);
						uint64_t dest_hbm_select = (dest_address / 64) % _mem_hbm_per_mc;
						req.lineAddr = dest_hbm_mc_address;
						req.cycle = _mcdram[dest_hbm_select]->access(req, 0, 4);
						// req.lineAddr = address;
						req.lineAddr = tmpAddr;
						total_latency += req.cycle;
						futex_unlock(&_lock);
						// std::cout << "workflow over here (XTA Hit ,is dram remapping to hbm)!!" << std::endl;
						return total_latency;
					}
				}
				else
				{ // 否则有可能是HBM,但也有可能是remap到DRAM
					auto it = HBMTable.find(address);
					if (it == HBMTable.end())
					{
						// 访问HBM，TODO
						req.lineAddr = mem_hbm_address;
						req.cycle = _mcdram[mem_hbm_select]->access(req, 0, 4);
						// req.lineAddr = address;
						req.lineAddr = tmpAddr;
						total_latency += req.cycle;
						// std::cout << "workflow over here (XTA Hit ,is hbm)!!" << std::endl;
						futex_unlock(&_lock);
						return total_latency;
					}
					else
					{
						uint64_t dest_address = it->second;
						// 访问DRAM,TODO
						req.lineAddr = dest_address;
						req.cycle = _ext_dram->access(req, 0, 4);
						total_latency += req.cycle;
						// req.lineAddr = address;
						req.lineAddr = tmpAddr;
						futex_unlock(&_lock);
						// std::cout << "workflow over here (XTA Hit ,is hbm remapping to dram)!!" << std::endl;
						return total_latency;
					}
				}
			}
		}
	} // ending look up XTA
	assert(0 != set_assoc_num);
	avg_temp = avg_temp / set_assoc_num;
	// std::cout << "workflow come here :(loop check) 000000 !" << std::endl;
	// XTA Miss 掉
	if (!if_XTA_hit)
	{
		// 根究 address 找到set 把set里的page 根据LRU值淘汰一个
		// 这个set 已经由之前的引用类型获得
		// 这里的address都有可能在remaptable里
		// std::cout << "workflow come here :(XTA Miss) !" << std::endl;
		int empty_idx = check_set_full(SETEntries);
		uint64_t lru_idx = ret_lru_page(SETEntries);

		// 表示没有空的，那就LRU干掉一个,这就有空的了
		// 被LRU干掉的数据根据迁移代价计算公式迁移到对应的内存介质
		// 基于ZSim的工作原理，流程都简化到两个RemapTable 以表示数据驱逐的地方
		if (-1 == empty_idx)
		{
			uint64_t cache_blk_num = 0;
			uint64_t dirty_blk_num = 0;

			for (uint32_t k = 0; k < hybrid2_blk_per_page; k++)
			{
				if (SETEntries[lru_idx].bit_vector[k])
					cache_blk_num++;
				if (SETEntries[lru_idx].dirty_vector[k])
					dirty_blk_num++;
			}
			uint64_t migrate_cost = 2 * hybrid2_blk_per_page - cache_blk_num + 1;
			uint64_t evict_cost = dirty_blk_num;
			uint64_t net_cost = migrate_cost - evict_cost;

			// uint64_t tmp_hybrid2_tag =  SETEntries[lru_idx]._hybrid2_tag;
			uint64_t tmp_hbm_tag = SETEntries[lru_idx]._hbm_tag;
			uint64_t tmp_dram_tag = SETEntries[lru_idx]._dram_tag;
			uint64_t heat_counter = SETEntries[lru_idx]._hybrid2_counter;

			bool migrate_init_dram = false;
			bool migrate_init_hbm = false;
			bool migrate_final_hbm = false;
			bool migrate_final_dram = false;

			// 是否迁移或逐出
			// 这一段是可能原来就在DRAM，或者被remap进HBM的部分
			// remap进HBM的部分，要是这部分数据不太热就踢出去
			if (address >= _mem_hbm_size)
			{
				migrate_init_dram = true;
				auto it = DRAMTable.find(address);
				if (it != DRAMTable.end())
				{
					migrate_init_dram = false;
					migrate_final_hbm = true;
				}

				// 如果迁移代价不高且原来就在DRAM里，就迁移
				if (migrate_init_dram && heat_counter > net_cost)
				{
					// 一直就在DRAM就加个映射
					// 依然是基于ZSim只需要返回延迟的假设,如果有对应的hbm_tag，不管DRAMTable有没有是不是，都改成新的映射
					if(tmp_hbm_tag != static_cast<uint64_t>(0)){
						DRAMTable[address] = tmp_hbm_tag * _hybrid2_page_size + blk_offset*64;
					}else{ // 否则按照地址均匀的方式，按地址%mem_hbm_size 映射
						DRAMTable[address] = address % _mem_hbm_size;
					}
				}

				// 否则就驱逐
				if (heat_counter <= net_cost)
				{
					// 可能是被映射进HBM的
					if (migrate_final_hbm)
					{
						// 解除映射
						DRAMTable.erase(address);
					} // 否则什么也不用做
				}
			}

			// 这一段是原来可能在HBM的部分，但是也有可能被remap进DRAM
			// 在HBM的话，只要数据比其它页面都冷就remap到DRAM（相当于踢掉）
			// 被remap进DRAM的话，只要数据比页面平均温度更热，就erase这个映射，相当于保持在HBM里
			if (address < _mem_hbm_size)
			{
				migrate_init_hbm = true;
				auto it = HBMTable.find(address);
				if (it != HBMTable.end())
				{
					migrate_init_hbm = false;
					migrate_final_dram = true;
				}

				// 最冷数据去掉了空数据（合理性待考察）
				if (migrate_init_hbm && heat_counter < low_temp)
				{
					// 映射到DRAM,不管有没有是不是，都更新成新映射；
					if(tmp_dram_tag != static_cast<uint64_t>(0)){
						HBMTable[address] = tmp_dram_tag * _hybrid2_page_size + blk_offset*64;
					}else{ // 否则按照地址，简单生成一个
						HBMTable[address] = address + (address % 7 + 1) * _mem_hbm_size;
					}
				}

				// 温热数据就留在HBM了
				if (migrate_final_dram && heat_counter >= avg_temp)
				{
					HBMTable.erase(address);
				}
			}

			// 置空
			SETEntries[lru_idx]._hybrid2_tag = 0;
			SETEntries[lru_idx]._hbm_tag = 0;
			SETEntries[lru_idx]._dram_tag = 0;
			SETEntries[lru_idx]._hybrid2_LRU = 0;
			SETEntries[lru_idx]._hybrid2_counter = 0;
			for (uint32_t k = 0; k < hybrid2_blk_per_page; k++)
			{
				SETEntries[lru_idx].bit_vector[k] = 0;
				SETEntries[lru_idx].dirty_vector[k] = 0;
			}

			// 更新索引
			empty_idx = lru_idx;
		}

		if (-1 != empty_idx) // 表示有空的，且一定不是-1：因为没有空的也会被我LRU干掉一个
		{
			SETEntries[empty_idx]._hybrid2_tag = get_page_id(address);
			SETEntries[empty_idx]._hybrid2_LRU = 0;
			SETEntries[empty_idx]._hybrid2_counter = 0;
			SETEntries[empty_idx]._hybrid2_counter += 1;

			if (type == STORE)
			{
				SETEntries[empty_idx].dirty_vector[static_cast<uint32_t>(blk_offset)] = 1;
			}

			// SETEntries[empty_idx].bit_vector[blk_offset] = 1; //这行修改的位置可能需要调整，理论上会出现一致性问题
			// 剩余需要根据address和remapTable进行更新

			uint64_t dest_blk_address = 0;
			bool is_dram = false;
			bool is_remapped = false;

			if (address > _mem_hbm_size)
			{ // 可能是dram，但也有可能是被remap的
				// std::cout << "workflow come here :(addr > memhbm_size)  !" << std::endl;
				SETEntries[empty_idx]._dram_tag = get_page_id(address);
				dest_blk_address = address;
				is_dram = true;
				// 看看有没有remap，有就更新，没有就不更新
				auto it = DRAMTable.find(address);
				if (it != DRAMTable.end()) // 就说明有对吧
				{
					SETEntries[empty_idx]._hbm_tag = get_page_id(it->second);
					dest_blk_address = it->second;
					is_dram = false;
					is_remapped = true;
				}
			}
			else // 否则有可能是HBM，有可能remap到dram
			{
				// std::cout << "workflow come here :(addr < memhbm_size)!" << std::endl;
				assert(address > 0);
				SETEntries[empty_idx]._hbm_tag = get_page_id(address);
				dest_blk_address = address;
				auto it = HBMTable.find(address);
				if (it != HBMTable.end()) // 就说明有对吧
				{
					SETEntries[empty_idx]._dram_tag = get_page_id(it->second);
					dest_blk_address = it->second;
					is_dram = true;
					is_remapped = true;
				}
			}

			// 这个时候基本的XTAEntry已经完成了，还差两个blk_vector
			// 看看dest_blk_address在哪里
			if (!is_dram)
			{ // 在HBM,只需访问HBM对应的blk
				// 访问HBM,TODO
				assert(static_cast<uint64_t>(0) != dest_blk_address);

				uint64_t dest_hbm_mc_address = (dest_blk_address / 64 / _mem_hbm_per_mc * 64) | (dest_blk_address % 64);
				uint64_t dest_hbm_select = (dest_blk_address / 64) % _mem_hbm_per_mc;
				req.lineAddr = dest_hbm_mc_address;
				req.cycle = _mcdram[dest_hbm_select]->access(req, 0, 4);

				// req.lineAddr = address;
				req.lineAddr = tmpAddr;
				total_latency += req.cycle;

				// 更新XTA
				SETEntries[empty_idx].bit_vector[static_cast<uint32_t>(blk_offset)] = 1;
				futex_unlock(&_lock);
				// req.lineAddr = tmpAddr;
				// std::cout << "workflow over here (!is_dram)!!" << std::endl;
				return total_latency;
			}
			else // 在DRAM
			{
				// 访问DRAM,TODO
				assert(static_cast<uint64_t>(0) != dest_blk_address);
				req.lineAddr = dest_blk_address;
				req.cycle = _ext_dram->access(req, 0, 4);
				// req.lineAddr = address;
				req.lineAddr = tmpAddr;
				total_latency += req.cycle;
				// 从DRAM写到HBM
				// 现在是Page有空的，然后原来的数据是在DRAM，所以DRAMTable需要更新进去这个remap,已经remap就无需管
				if (!is_remapped)
				{
					// 在ZSim中是否由于access只返回访问对应内存介质access的延迟，而不会产生实际的修改内存操作
					// 基于这样的设想，我是否只需要remap到HBM的对应set的任何一个有效位置即可呢？
					// 再更新XTA
					uint64_t dest_hbm_addr = address % _mem_hbm_size;
					DRAMTable[address] = dest_hbm_addr;
					SETEntries[empty_idx]._hybrid2_counter += 1;
					SETEntries[empty_idx].bit_vector[static_cast<uint32_t>(blk_offset)] = 1;
				}
				futex_unlock(&_lock);
				// req.lineAddr = tmpAddr;
				// std::cout << "workflow over here (is_dram)!!" << std::endl;
				return total_latency;
			}
		}
	}

	return 0;
}

/**
 * ISA_ALLOC ?? ISA_FREE ??
 */
uint64_t
MemoryController::chameleon_access(MemReq& req)
{
	Address tmpAddr = req.lineAddr;
	req.lineAddr = vaddr_to_paddr(req);
	switch (req.type) {
        case PUTS:
        case PUTX:
            *req.state = I;
            break;
        case GETS:
            *req.state = req.is(MemReq::NOEXCL)? S : E;
            break;
        case GETX:
            *req.state = M;
            break;
        default: panic("!?");
    }
	// 干净数据，不会改变块的状态
	if (req.type == PUTS){
		return req.cycle;
	}

	ReqType type = (req.type == GETS || req.type == GETX)? LOAD : STORE;
	Address address = req.lineAddr;
	address = address / 64 * 64; // align to 64B cacheline

	uint64_t seg_idx = get_segment(address); // idx in segGrp
	int seg_num = get_segment_num(address); // hbm 0; ddr 1 - n;
	int blk_offset = address % _mem_hbm_size / _chameleon_blk_size;
	int cacheline_offset = (address % _chameleon_blk_size)/64;

	uint32_t mem_hbm_select = (address / 64) % _cache_hbm_per_mc;
	Address mem_hbm_address = (address / 64 /_cache_hbm_per_mc * 64) | (address % 64);

	// 找到这个segGrpEntry
	segGrpEntry* entryPtr = &segGrps[seg_idx];
	bool is_cache = entryPtr->isCache();

	uint64_t wait_latency = 0; // 遇到忙碌的等待时间，按照排队论的Little's Law （ L = lamda * W）,而不是固定延迟

	// 根据程序执行流，先去分开READ WRITE
	if(type == LOAD)
	{
		// Follow the paper [ISA-Alloc Opt]
		// paper (2) : check if it exists remapping operation
		if(entryPtr->remapVector[seg_num] == seg_num) // without remapping
		{
			// paper (3) : check if it is HBM; [P] -> Seg 0
			if(address < _mem_hbm_size) // 也可以写成 seg_num == 0 (This branch: seg_num=0)
			{
				// check ABV Bits
				bool ABVZeroflag = false;
				int ABVZero_idx = -1; // Q
				for(int i = 1; i < entryPtr->ddrNum + 1 ; i++)
				{
					if(entryPtr->ABV[i] == 0)
					{
						ABVZeroflag = true;
						ABVZero_idx = i;
						break;
					}
				}

				// paper (4) : check ABVZeroFlag
				if(ABVZeroflag)
				{
					// paper (7) : P -> Q ; Q -> P
					entryPtr->remapVector[ABVZero_idx] = seg_num;
					entryPtr->remapVector[seg_num] = ABVZero_idx;
					// paper (8)
					entryPtr->ABV[0] = 1;
					bool allABVBusy = true;
					for(int i = 1; i < entryPtr->ddrNum + 1 ; i++)
					{
						if(entryPtr->ABV[i] == 0)
						{
							allABVBusy = false;
							break;
						}
					}
					// paper (10)
					if(allABVBusy)
					{
						// turn to POM Paper(6)
						entryPtr->setCacheMode(false);
					}
					// else wo do nothing  paper(11)
				}
				else // No Remain Space , turn to POM , paper (5,6)
				{
					entryPtr->ABV[0] = 1;
					entryPtr->setCacheMode(false);
				}
			}
			else // Notice : DRAM seg_num > 0 (This branch: seg_num > 0) [P] Seg > 0
			{
				// Paper (8)
				entryPtr->ABV[seg_num] = 1;
				// Paper (10)
				bool allABVBusy = true;
				for(int i = 0; i < entryPtr->ddrNum + 1 ; i++)
				{
					if(entryPtr->ABV[i] == 0)
					{
						allABVBusy = false;
						break;
					}
				}
				if(allABVBusy)
				{   // Paper(6)
					entryPtr->setCacheMode(false);
				}
				// else we do nothing paper(11)
			}
		}
		else // exist remapping
 		{
			// allocate P to destination segment Paper (9) xxx???

			// Paper (8)
			entryPtr->ABV[entryPtr->remapVector[seg_num]] = 1;
			// Paper (10)
			bool allABVBusy = true;
			for(int i = 0; i < entryPtr->ddrNum + 1 ; i++)
			{
				if(entryPtr->ABV[i] == 0)
				{
					allABVBusy = false;
					break;
				}
			}
			if(allABVBusy)
			{   // Paper(6)
				entryPtr->setCacheMode(false);
			} //  else we do nothing paper(11)
		}

		// Follow the paper [ISA-Free Opt] Paper(2)
		bool is_cache = entryPtr->isCache();
		if(entryPtr->remapVector[seg_num] == seg_num)
		{
			// Paper (3)
			// Paper(12)
			if(address < _mem_hbm_size) // [P] Seg 0
			{
				entryPtr->ABV[0] = 0;
				// paper (13)
				if(!is_cache)
				{
					// paper(15)
					entryPtr->setDirty(false);
				}// else we do nothing paper(14)
			}
			else // paper(4) [P] Seg > 0
			{
				entryPtr->ABV[seg_num] = 0;
				// paper (5)
				if(!is_cache)
				{
					// xxxxx??????
					// remap
					entryPtr->remapVector[0] = seg_num;
					entryPtr->remapVector[seg_num] = 0;
					// turn to cache mode
					entryPtr->setCacheMode(true);
					entryPtr->setDirty(true);
				}// else we do nothing paper(6)
			}
		}
		else // paper(8)
		{
			// trace the remapped P
			int ori_idx = -1;
			for(int i =0; i<entryPtr->ddrNum + 1;i++)
			{
				if(entryPtr->remapVector[i] == seg_num)
				{
					ori_idx = i;
					break;
				}
			}
			assert(-1 != ori_idx);

			// paper (9)
			if(ori_idx == 0) // hbm , paper(11)
			{
				entryPtr->ABV[ori_idx] = 0;
				if(!is_cache) // paper (13)
				{
					entryPtr->setCacheMode(true);
					entryPtr->setDirty(false);// paper (15)
				}//else we do nothing paper(14)
			}
			else // dram paper(10)
			{
				entryPtr->ABV[ori_idx] = 0;
				// paper (7)
				entryPtr->remapVector[0] = ori_idx;
				entryPtr->remapVector[ori_idx] = 0;
				entryPtr->setCacheMode(true);
				entryPtr->setDirty(false);
			}
		}

	}
	else
	{

	}
}


// bumblebee cHBM mHBM exclusive ?
// mHBM -> cHBM -> DRAM
// ToDo : 基于热度的重映射分配机制
//		  如果最近分配的页面仍然驻留在热表队列中，并且有空闲的HBM空间可用，则该页面分配到HBM。否则，该页面应分配到片外DRAM。

uint64_t
MemoryController::bumblebee_access(MemReq& req)
{
	switch (req.type)
	{
	case PUTS:
	case PUTX:
		*req.state = I;
		break;
	case GETS:
		*req.state = req.is(MemReq::NOEXCL) ? S : E;
		break;
	case GETX:
		*req.state = M;
		break;
	default:
		panic("!?");
	}
	
	if (req.type == PUTS)
	{
		return req.cycle;
	}
	ReqType type = (req.type == GETS || req.type == GETX) ? LOAD : STORE;
	Address tmpAddr = req.lineAddr;
	req.lineAddr = vaddr_to_paddr(req);
	Address address = req.lineAddr;
	// address = address / 64 * 64;
	uint32_t mem_hbm_select = (address / 64) % _mem_hbm_per_mc;
	// assert(4 > mem_hbm_select);
	Address mem_hbm_address = (address / 64 / _mem_hbm_per_mc * 64) | (address % 64);


	// get set and page offset in set; it's easy, so this function is not decoupled;
	uint64_t set_id = -1;
	int page_offset = -1;
	int blk_offset = -1;
	bool is_hbm = false;

	if(address < _mem_hbm_size)
	{
		set_id = address /  _bumblebee_page_size / bumblebee_n;
		page_offset =  address /  _bumblebee_page_size % bumblebee_n;
		blk_offset = address % _bumblebee_page_size % _bumblebee_blk_size;
		is_hbm = true;
	}
	else
	{
		set_id = (address - _mem_hbm_size) / _bumblebee_page_size / bumblebee_m;
		// 相对于set的page offset
		page_offset =  bumblebee_n + (address - _mem_hbm_size) /  _bumblebee_page_size % bumblebee_m;
		blk_offset = address % _bumblebee_page_size % _bumblebee_blk_size;
	}

	assert(-1 != set_id);
	assert(-1 != page_offset);

	PLEEntry pleEntry =  MetaGrp[set_id]._pleEntry;
	g_vector<BLEEntry> bleEntries =  MetaGrp[set_id]._bleEntries;
	HotnenssTracker hotTracker = HotnessTable[set_id];
	uint64_t current_cycle = req.cycle;

	// 立即更新BLE状态
	for(int i = 0;i<bumblebee_m+bumblebee_n;i++)
	{
		if(bleEntries[i].ple_idx == page_offset)
		{
			bleEntries[i].validVector[blk_offset] = 1;
			if(type == STORE)bleEntries[i].dirtyVector[blk_offset] = 1;
		}
	}


	// search value(new PLE)
	int search_idx = -1;
	int occpu = -1;
	int page_type = -1;
	for(int i = 0; i < (bumblebee_m + bumblebee_n);i++)
	{
		// find page
		if(pleEntry.PLE[i] == page_offset)
		{
			search_idx = i;
			occpu = pleEntry.Occupy[i];
			page_type = pleEntry.Type[i];
			// 是有可能出现重复的，由于HBM在低地址，可能cache住了DRAM的数据，所以需要break;
			break;
		}
	}

	// PRT Miss
	if(-1 == search_idx)
	{
		// allocate ToDo：基于热度分配和空闲页面分配 可解耦一个函数
		// 如果最近分配的页面仍然驻留在热表队列中，并且有空闲的HBM空间可用，则该页面分配到HBM。否则，该页面应分配到片外DRAM。
		// 论文说的轻巧，怎么定义最近分配的页面？ 
		// HotTable 又是LRU的 最近访问的肯定不会先弹出去啊 ？

		// 先根据空闲的HBM来吧
		int free_idx = -1;
		for(int i = 0;i < bumblebee_n ;i++)
		{
			if(pleEntry.Occupy[i]==0)
			{
				free_idx = i;
			}
		}

		// Notes:相当于所有的page都会一一对应，因此如果PRT Miss，则对应的set 必定存在空的page slot
		// 		 且如果是DDR,则对应的DDR Page Slot一定未分配

		// 有空闲HBM
		if(-1 != free_idx)
		{
			// 先分配(元数据一起修改)
			// 最好还是不remap，先看一下原始的page_offset是否是HBM且free
			if(page_offset < bumblebee_n && pleEntry.Occupy[page_offset]==0) free_idx = page_offset;
			pleEntry.PLE[free_idx] = page_offset;
			pleEntry.Occupy[free_idx] = 1;
			pleEntry.Type[free_idx] = 1; // HBM

			// 这个页表加入HBMQueue
			QueuePage _queuePage;
			_queuePage._page_id = page_offset; 
			_queuePage._counter += 1;
			_queuePage._last_mod_cycle = req.cycle;
			hotTracker.HBMQueue.push_front(_queuePage);
			hotTracker._rh = 0;
			for(int i = 0;i<bumblebee_n;i++)
			{
				if(pleEntry.Occupy[i] == 1)
				{
					hotTracker._rh += 1;
				}
			}
			// hotTracker._rh = (hotTracker._rh  + 1) / bumblebee_n;
			hotTracker._na += 1;

			// 所有HBM被占用,所有cHBM转变为mHBM
			if(hotTracker._rh == bumblebee_n)
			{
				for(int i = 0;i < bumblebee_n;i++)
				{
					if(pleEntry.Type[i] == 2)
					{
						pleEntry.PLE[i] = -1;
						pleEntry.Type[i] = 1;
						pleEntry.Occupy[i] = 0;
					}
				}
			}

			// Access Memory
			req.lineAddr = mem_hbm_address;
			req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
			req.lineAddr = tmpAddr;

			// 看看是否要驱逐（支持的逻辑是我只有往HBMQueue新增Page，才有可能使得HBM占用比之前高）
			// 驱逐逻辑是在HBM占用率较高的情况下，HBM LRU Table 的计数器长时间保持不变
			if(hotTracker._rh >= rh_upper)
			{
				tryEvict(pleEntry,hotTracker,current_cycle);
			}
			// else // rh < upper HBM占用不高
			// {

			// } 

		}
		else // 没有空闲HBM
		{
			// 原来是DDR
			if(page_offset > bumblebee_n)
			{
				// 原来的未被占用
				// 状态位修改：DRAMQueue
				if(pleEntry.Occupy[page_offset]==0)
				{
					pleEntry.PLE[page_offset] = page_offset;
					pleEntry.Occupy[page_offset] = 1;
					pleEntry.Type[page_offset] = 0;

					QueuePage _ddr_page;
					_ddr_page._page_id=page_offset;
					_ddr_page._counter=1;
					_ddr_page._last_mod_cycle=current_cycle;
					hotTracker.DRAMQueue.push_front(_ddr_page);
				}
				else // 原来的被占用  状态位修改：DRAMQueue
				{ 
					int free_ddr = -1;
					for(int i = bumblebee_n; i <bumblebee_m + bumblebee_n;i++)
					{
						if(pleEntry.Occupy[i] == 0)
						{
							free_ddr = i;
							break;
						}
					}
					if(free_ddr != -1)
					{
						pleEntry.PLE[free_idx] = page_offset;
						pleEntry.Occupy[free_idx] = 1;
						pleEntry.Type[free_idx] = 0;
						
					}
					else
					{
						// 理论上是不会走到这里的
						pleEntry.PLE[page_offset] = page_offset;
						pleEntry.Occupy[page_offset] = 1;
						pleEntry.Type[page_offset] = 0;	
					}
					QueuePage _ddr_page;
					_ddr_page._page_id=page_offset;
					_ddr_page._counter=1;
					_ddr_page._last_mod_cycle=current_cycle;
					hotTracker.DRAMQueue.push_front(_ddr_page);
				}
			}
			else
			{
				bool should_find_ddr = false;
				// 原来不是DDR，是HBM，而HBM全被占用
				// 查看对应状态
				if(pleEntry.Type[page_offset] == 2)
				{
					// cHBM
					// 看看原来cache的是DDR还是HBM

					if(pleEntry.PLE[page_offset] > bumblebee_n)
					{
						// DDR 直接写回（有dirty）置空（如果dirty）
						pleEntry.PLE[page_offset] = page_offset;
						pleEntry.Occupy[page_offset] = 1;
						pleEntry.Type[page_offset] = 2;	

						QueuePage _hbm_page;
						_hbm_page._page_id = page_offset;
						_hbm_page._counter = 1;
						_hbm_page._last_mod_cycle = current_cycle;
						hotTracker.HBMQueue.push_front(_hbm_page);
						// 原来都没有触发HBMQueue
					}
					else
					{
						// 否则看看DDR是否有空
						should_find_ddr = true;
					}
				}
				else
				{
					should_find_ddr = true;
				}

				if(should_find_ddr)
				{
					int free_ddr = -1;
					for(int i = bumblebee_n;i<bumblebee_m +bumblebee_n;i++)
					{
						if(pleEntry.Occupy[i]==0)
						{
							free_ddr = i;
							break;
						}
					}

					if(-1 == free_ddr)
					{
						// DDR HBM全满，所有cHBM 变为 mHBM

						// 状态位修改： nc na nn
						int turn_hbm_idx = -1;
						for(int i = 0;i<bumblebee_n;i++)
						{
							if(pleEntry.Type[i]==2)
							{
								pleEntry.Type[i] = 1;
								turn_hbm_idx = i;
							}
						}
						if(turn_hbm_idx == -1)
						{
							// 这是不可能的assert(false)
							// 如果遇到了，direct mapping
							pleEntry.PLE[page_offset] = page_offset;
							pleEntry.Occupy[page_offset] = 1;
							pleEntry.Type[page_offset] = 1;
						}
						else
						{
							pleEntry.PLE[turn_hbm_idx] = page_offset;
							pleEntry.Occupy[turn_hbm_idx] = 1;
							pleEntry.Type[turn_hbm_idx] = 1;
						}

						hotTracker._rh=0;
						hotTracker._nc=0;
						hotTracker._na=0;
						hotTracker._nn=0;

						for(int i=0;i<bumblebee_n;i++)
						{
							if(pleEntry.Type[i]==2)assert(false);
							if(pleEntry.Type[i]==1 && pleEntry.Occupy[i]==1)hotTracker._na += 1;
							if(pleEntry.Type[i]==1 && pleEntry.Occupy[i]==0)hotTracker._nn += 1;
							if(pleEntry.Occupy[i]==1)hotTracker._rh += 1;
						}

						QueuePage _hbm_page;
						_hbm_page._page_id = page_offset;
						_hbm_page._counter = 1;
						_hbm_page._last_mod_cycle = current_cycle;
						hotTracker.HBMQueue.push_front(_hbm_page);
						// 这么高占用早就该判断是否踢掉了
						// 到时候把这一段抽象出来吧
						if(hotTracker._rh > rh_upper)tryEvict(pleEntry,hotTracker,current_cycle);
					}
					else
					{
						// 分配到对应DDR
						pleEntry.PLE[free_ddr] = page_offset;
						pleEntry.Occupy[free_ddr] = 1;
						pleEntry.Type[free_ddr] = 0;

						QueuePage _ddr_page;
						_ddr_page._page_id=page_offset;
						_ddr_page._counter=1;
						_ddr_page._last_mod_cycle=current_cycle;
						hotTracker.DRAMQueue.push_front(_ddr_page);
					}
				}
			}
		}
		

		// 分配完，看看现在到底是在什么介质里
		int dest_idx = -1;
		for(int i = 0;i<bumblebee_m +bumblebee_n;i++)
		{
			if(pleEntry.PLE[i]==page_offset)
			{
				dest_idx = i;
			}
		}

		is_hbm = dest_idx > bumblebee_n ? false:true;

		if(is_hbm)
		{
			req.lineAddr = mem_hbm_address;
			req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
			req.lineAddr = tmpAddr;

			// update metadata

			futex_unlock(&_lock);
			return req.cycle;
		}
		else
		{
			req.cycle = _ext_dram->access(req,0,4);
			req.lineAddr = tmpAddr;

			// update metadata

			futex_unlock(&_lock);
			return req.cycle;
		}

	}

	// // PRT Hit
	// // idx = search_idx
	// // 根据idx 判断处于哪一种介质上
	// if(search_idx < bumblebee_n)
	// {
	// 	// HBM
	// 	// Case1: mHBM Direct access; 
	// 	// Case2: cHBM (cache ddr page)  -> (1)blk hit  (2) blk miss

	// 	if(2 == page_type)
	// 	{
	// 		//cHBM
	// 		bool blk_hit = false;
	// 		for(int i = 0;i < (bumblebee_m + bumblebee_n);i++)
	// 		{
	// 			if(bleEntry.ple_idx == search_idx)
	// 			{
	// 				if(bleEntry.validVector[blk_offset] == 1)blk_hit=true;
	// 				else blk_hit = false;
	// 				break;
	// 			}
	// 		}

	// 		if(blk_hit)
	// 		{
	// 			req.lineAddr =  mem_hbm_address;
	// 			req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
	// 			req.lineAddr = tmpAddr;

	// 			// update metadata

	// 			futex_unlock(&_lock);
	// 			return req.cycle;
	// 		}
	// 		else
	// 		{
	// 			req.cycle = _ext_dram->access(req,0,4);
	// 			req.lineAddr = tmpAddr;

	// 			// update metadata

	// 			futex_unlock(&_lock);
	// 			return req.cycle;
	// 		}
	// 	}
	// 	else if(1 == page_type)
	// 	{
	// 		// mHBM
	// 		req.lineAddr =  mem_hbm_address;
	// 		req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
	// 		req.lineAddr = tmpAddr;

	// 		// update metadata

	// 		futex_unlock(&_lock);
	// 		return req.cycle;
	// 	}
	// }
	// else
	// {
	// 	// DRAM
	// 	req.cycle = _ext_dram->access(req,0,4);
	// 	req.lineAddr = tmpAddr;

	// 	// update metadata

	// 	futex_unlock(&_lock);
	// 	return req.cycle;
	// }

	// 	执行流不应该执行到这里才返回
	assert(false);
	req.lineAddr = tmpAddr;
	return req.cycle;
}

void
MemoryController::tryEvict(PLEEntry& pleEntry,HotnenssTracker& hotTracker,uint64_t current_cycle)
{
	QueuePage endPage = hotTracker.HBMQueue.back();
	int endPageOffset = endPage._page_id;
	// 根据value 找到 idx
	int endPageIdx = -1;
	for(int i = 0;i < bumblebee_m + bumblebee_n;i++)
	{
		if(endPageOffset == pleEntry.PLE[i])
		{
			endPageIdx = i;
			break;
		}
	}
	assert(endPageIdx != -1);
	int end_page_type = pleEntry.Type[endPageIdx];
	int end_page_busy = pleEntry.Occupy[endPageIdx];

	if(current_cycle - endPage._last_mod_cycle > long_time)
	{
		// 僵尸页面
		QueuePage swap_page;
		bool swap_empty = true;
		uint64_t swap_value = -1;
		uint64_t swap_idx = -1;

		if(hotTracker.DRAMQueue.size() > 0)
		{
			swap_page = hotTracker.DRAMQueue.front();  // 先把DRAMQueue 的 front取出来
			swap_empty = false;
		}

		if(!swap_empty)
		{
			swap_value = swap_page._page_id;
			for(int i = 0;i< bumblebee_m + bumblebee_n;i++)
			{
				if(swap_value == pleEntry.PLE[i])
				{
					swap_idx = i;
					break;
				}
			}
		}

		// 驱逐操作
		bool ddr_empty = false;
		int ddr_empty_idx = -1;
		for(int i =bumblebee_n ; i<bumblebee_m+bumblebee_n;i++)
		{
			if(pleEntry.Occupy[i]==0)
			{
				ddr_empty = true;
				ddr_empty_idx = i;
			}
		}

		if(ddr_empty) // ddr有空
		{
			// 如果是cacheHBM,直接分配空DDR
			// // 状态变化为：cHBM未占用未分配，nc - 1,rh - 1
			if(end_page_type == 2)
			{
				pleEntry.PLE[ddr_empty_idx] = endPageOffset;
				pleEntry.Occupy[ddr_empty_idx] = 1;
				pleEntry.Type[ddr_empty_idx] = 0;

				hotTracker.HBMQueue.pop_back();
				hotTracker.DRAMQueue.push_front(endPage);

				pleEntry.PLE[endPageIdx] = -1;
				pleEntry.Occupy[endPageIdx] = 0;
				pleEntry.Type[endPageIdx] = 2; // keep cache mode ,but no allocated page
			}
			else // ddr有空，但是endPage是MHBM。缓冲一下，MHBM=>CHBM 状态变化为：mHBM - 1;cHBM + 1
			{
				pleEntry.Type[endPageIdx] = 2;
				pleEntry.Occupy[endPageIdx] = 1;

				hotTracker._na = 0;
				hotTracker._nn = 0;
				hotTracker._nc = 0;

				for(int i = 0;i<bumblebee_n;i++)
				{
					// mHBM被访问
					if(pleEntry.Occupy[i]==1 && pleEntry.Type[i]==1)
					{
						hotTracker._na += 1;
					}
					// mHBM未被访问
					if(pleEntry.Occupy[i]==0 && pleEntry.Type[i]==1)
					{
						hotTracker._nn += 1;
					}
					if(pleEntry.Type[i] == 2)
					{
						hotTracker._nc += 1;
					}
				}
			}
		}
		else // ddr非空
		{
			// get swap page 已经得到了
			// 判断是否为空，已经判断了
			if(!swap_empty)
			{
				// 不存在swap page （理论上概率极低，但存在可能，采取随机swap）
				// DDR没有空的 DRAMQueue又没有元素 想想就是小概率事件
				// 状态位变化，ple,

				int random_idx = current_cycle % bumblebee_m + bumblebee_n - 1;
				pleEntry.PLE[endPageIdx] = pleEntry.PLE[random_idx];
				pleEntry.Occupy[endPageIdx] = pleEntry.Occupy[random_idx];

				pleEntry.PLE[random_idx] = endPage._page_id;
				pleEntry.Occupy[random_idx] = 1;
			}
			else
			{
				// swap page存在
				// endPage的温度是否超过swap page的温度？
				if(endPage._counter < swap_page._counter)
				{
					// 即将evict的page比另一个页面冷才会evict
					if(end_page_type == 2)
					{
						// 如果是cache mode
						// 如果cache的是DDR 写回即可
						// 状态变化：cHBM不占用，rh - 1，nc - 1 
						if(endPageOffset > bumblebee_n)
						{
							pleEntry.PLE[endPageIdx] = -1;
							pleEntry.Occupy[endPageIdx] = 0;
							pleEntry.Type[endPageIdx] = 2;

							hotTracker._rh -= 1;
							hotTracker._nc -= 1;

							hotTracker.HBMQueue.pop_back();
						}
						else // 如果cache的是HBM 需要和swap page swap
						{
							// 状态位变化:ple相关状态变化，hotTracked保持不变
							pleEntry.PLE[endPageIdx] = pleEntry.PLE[swap_idx];
							pleEntry.Occupy[endPageIdx] = pleEntry.Occupy[swap_idx];

							pleEntry.PLE[swap_idx]=endPageOffset;
							pleEntry.Occupy[swap_idx] = 1;

							hotTracker.HBMQueue.pop_back();
							hotTracker.DRAMQueue.pop_front();
							hotTracker.DRAMQueue.push_front(endPage);
							hotTracker.HBMQueue.push_front(swap_page);
						}
					}
					else
					{
						// 只能是mem模式 缓冲一下 mHBM => cHBM
						// 状态位变化：ple,nc + 1,mHBM -1 
						pleEntry.Type[endPageIdx] = 2;
						pleEntry.Occupy[endPageIdx] = 1;
						hotTracker._na = 0;
						hotTracker._nn = 0;
						hotTracker._nc = 0;

						for(int i = 0;i<bumblebee_n;i++)
						{
							// mHBM被访问
							if(pleEntry.Occupy[i]==1 && pleEntry.Type[i]==1)
							{
								hotTracker._na += 1;
							}

							// mHBM未被访问
							if(pleEntry.Occupy[i]==0 && pleEntry.Type[i]==1)
							{
								hotTracker._nn += 1;
							}

							if(pleEntry.Type[i] == 2)
							{
								hotTracker._nc += 1;
							}
						}
					}
				}
				// else {} 不驱逐，就什么也不用做
			}
		} 
	}// else {} 不是僵尸页面
}

uint64_t
MemoryController::get_segment(Address addr)
{
	return addr % _mem_hbm_size / _chameleon_blk_size;
}

int
MemoryController::get_segment_num(Address addr)
{
	return addr / _mem_hbm_size;
}

/**
 * Only for test !
 */
uint64_t
MemoryController::random_hybrid2_access(MemReq req)
{
	// futex_lock(&_lock);
	switch (req.type)
	{
	case PUTS:
	case PUTX:
		*req.state = I;
		break;
	case GETS:
		*req.state = req.is(MemReq::NOEXCL) ? S : E;
		break;
	case GETX:
		*req.state = M;
		break;
	default:
		panic("!?");
	}
	// 干净数据，不会改变块的状态
	if (req.type == PUTS)
	{
		return req.cycle;
	}
	// futex_unlock(&_lock); 这里的执行已经持有了这把锁，由于在access中直接返回
	// 在本回合（快要）结束（的某个时机），需要释放这把锁

	// 请求状态
	// ReqType type = (req.type == GETS || req.type == GETX)? LOAD : STORE;
	// 表示的地址
	Address address = req.lineAddr;
	// HBM在这里需要自己考虑分到哪一个通道，但是ZSim不太涉及请求排队

	// uint32_t cache_hbm_select = (address / 64) % _cache_hbm_per_mc;
	// Address cache_hbm_address = (address / 64 /_cache_hbm_per_mc * 64) | (address % 64);
	assert(0 != _cache_hbm_per_mc);
	uint32_t mem_hbm_select = (address / 64) % _cache_hbm_per_mc;
	// assert(4 > mem_hbm_select);
	Address mem_hbm_address = (address / 64 / _cache_hbm_per_mc * 64) | (address % 64);

	// 创建一个随机数生成器
	std::random_device rd;						 // 用于生成种子
	std::mt19937 gen(rd());						 // 使用梅森旋转算法生成随机数
	std::uniform_int_distribution<> dis(1, 100); // 生成范围为[1, 100]的均匀分布

	// 生成一个随机整数
	int random_num = dis(gen);

	if (random_num % 2 == 0)
	{
		// std::cout << " _ext_dram.req.cycle " << _ext_dram->access(req,0,4) << std::endl;
		req.cycle = _ext_dram->access(req, 0, 4);
		// futex_unlock(&_lock);
		return req.cycle;
	}
	else
	{

		Address tmp = req.lineAddr;
		req.lineAddr = mem_hbm_address;

		req.cycle = _mcdram[mem_hbm_select]->access(req, 0, 4);

		req.lineAddr = tmp;

		return req.cycle;
		// req.cycle = _ext_dram->access(req,0,4);
		// futex_unlock(&_lock);
		// return req.cycle;
	}
}

/**
 * Only for test !
 */
uint64_t
MemoryController::hbm_hybrid2_access(MemReq req)
{
	// futex_lock(&_lock);
	// switch (req.type) {
	//     case PUTS:
	//     case PUTX:
	//         *req.state = I;
	//         break;
	//     case GETS:
	//         *req.state = req.is(MemReq::NOEXCL)? S : E;
	//         break;
	//     case GETX:
	//         *req.state = M;
	//         break;
	//     default: panic("!?");
	// }
	// // 干净数据，不会改变块的状态
	// if (req.type == PUTS){
	// 	return req.cycle;
	// }

	Address address = req.lineAddr;
	uint32_t mcdram_select = (address / 64) % _mcdram_per_mc;
	Address mc_address = (address / 64 / _mcdram_per_mc * 64) | (address % 64);

	req.lineAddr = mc_address;
	req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
	// req.cycle = _memhbm[mcdram_select]->access(req, 0, 4);
	req.lineAddr = address;
	return req.cycle;
}

// 这段代码写的分类，实际上没这个必要，但是为了以后有可能改assoc，预留了分类
uint64_t
MemoryController::get_set_id(uint64_t addr)
{
	if (addr < _mem_hbm_size)
	{
		uint64_t pg_id = get_page_id(addr);
		return pg_id % hbm_set_num;
	}
	else // 按照内存无限的假定，可以有模拟器设置global memory
	{
		assert(addr >= _mem_hbm_size);
		uint64_t pg_id = get_page_id(addr);
		return pg_id % hbm_set_num;
	}
}

uint64_t
MemoryController::get_page_id(uint64_t addr)
{
	return addr / _hybrid2_page_size;
}

g_vector<MemoryController::XTAEntry> &
MemoryController::find_XTA_set(uint64_t set_id)
{
	assert(set_id < XTA.size() && set_id >= 0);
	return XTA[set_id];
}

uint64_t
MemoryController::ret_lru_page(g_vector<XTAEntry> SETEntries)
{
	if (SETEntries.empty())
	{
		return -1;
	}
	uint64_t uint_max = SETEntries.size() + 114514;
	int max_lru = -1;
	uint64_t max_idx = uint_max;
	for (auto i = 0u; i < SETEntries.size(); i++)
	{
		if (max_lru < (int)SETEntries[i]._hybrid2_LRU)
		{
			max_lru = (int)SETEntries[i]._hybrid2_LRU;
			max_idx = (uint64_t)i;
		}
	}
	assert(max_idx != uint_max);
	return max_idx;
}

int MemoryController::check_set_full(g_vector<XTAEntry> SETEntries)
{
	int empty_idx = -1;
	for (auto i = 0u; i < SETEntries.size(); i++)
	{
		if (static_cast<uint64_t>(0) == SETEntries[i]._hybrid2_tag)
		{
			empty_idx = i;
			break;
		}
	}
	return empty_idx;
}

Address
MemoryController::vaddr_to_paddr(MemReq req)
{
	Address vLineAddr = req.lineAddr;

	uint64_t page_offset = vLineAddr & (_hybrid2_page_size - 1);
	uint64_t page_bits = std::log2(_hybrid2_page_size);
	uint64_t vpgnum = vLineAddr >> page_bits;

	uint64_t ppgnum = fixedMapping[vpgnum % num_pages];
	Address pLineAddr = (ppgnum << page_bits) | page_offset;
	// Add this code, then can run smoothly, but how to correct the logic
	// pLineAddr += 1024 * 1024 * 1024;

	return pLineAddr;
};

Address
MemoryController::paddr_to_vaddr(Address pLineAddr)
{
	uint64_t page_bits = std::log2(_hybrid2_page_size);
	uint64_t page_offset = pLineAddr & (_hybrid2_page_size - 1);
	uint64_t ppgnum = pLineAddr >> page_bits;

	uint64_t vpgnum = 0;
	for (uint64_t i = 0; i < num_pages; ++i)
	{
		if (fixedMapping[i] == ppgnum)
		{
			vpgnum = i; // 找到对应的虚拟页号
			break;
		}
	}

	Address vLineAddr = (vpgnum << page_bits) | page_offset;
	return vLineAddr;
}

bool MemoryController::is_hbm(MemReq req)
{
	bool is_hbm = true;
	Address pLineAddr = vaddr_to_paddr(req);
	if (pLineAddr >= _mem_hbm_size)
	{
		is_hbm = false;
	}
	return is_hbm;
}

// In test , bug occured without calling this function
// Following function has been removed from above code !
Address
MemoryController::handle_low_address(Address addr)
{
	if (addr >= 0 && addr < 1024 * 1024)
	{
		addr = addr + 1024 * 1024;
	}
	return addr;
}

DDRMemory *
MemoryController::BuildDDRMemory(Config &config, uint32_t frequency,
								 uint32_t domain, g_string name, const string &prefix, uint32_t tBL, double timing_scale)
{
	uint32_t ranksPerChannel = config.get<uint32_t>(prefix + "ranksPerChannel", 4);
	uint32_t banksPerRank = config.get<uint32_t>(prefix + "banksPerRank", 8);					 // DDR3 std is 8
	uint32_t pageSize = config.get<uint32_t>(prefix + "pageSize", 8 * 1024);					 // 1Kb cols, x4 devices
	const char *tech = config.get<const char *>(prefix + "tech", "DDR3-1333-CL10");				 // see cpp file for other techs
	const char *addrMapping = config.get<const char *>(prefix + "addrMapping", "rank:col:bank"); // address splitter interleaves channels; row always on top

	// If set, writes are deferred and bursted out to reduce WTR overheads
	bool deferWrites = config.get<bool>(prefix + "deferWrites", true);
	bool closedPage = config.get<bool>(prefix + "closedPage", true);

	// Max row hits before we stop prioritizing further row hits to this bank.
	// Balances throughput and fairness; 0 -> FCFS / high (e.g., -1) -> pure FR-FCFS
	uint32_t maxRowHits = config.get<uint32_t>(prefix + "maxRowHits", 4);

	// Request queues
	uint32_t queueDepth = config.get<uint32_t>(prefix + "queueDepth", 16);
	uint32_t controllerLatency = config.get<uint32_t>(prefix + "controllerLatency", 10); // in system cycles

	auto mem = (DDRMemory *)gm_malloc(sizeof(DDRMemory));
	new (mem) DDRMemory(zinfo->lineSize, pageSize, ranksPerChannel, banksPerRank, frequency, tech, addrMapping, controllerLatency, queueDepth, maxRowHits, deferWrites, closedPage, domain, name, tBL, timing_scale);
	printf("GET MEM INFO : %d %d", zinfo->lineSize, pageSize);
	return mem;
}

void MemoryController::initStats(AggregateStat *parentStat)
{
	AggregateStat *memStats = new AggregateStat();
	memStats->init(_name.c_str(), "Memory controller stats");

	_numPlacement.init("placement", "Number of Placement");
	memStats->append(&_numPlacement);
	_numCleanEviction.init("cleanEvict", "Clean Eviction");
	memStats->append(&_numCleanEviction);
	_numDirtyEviction.init("dirtyEvict", "Dirty Eviction");
	memStats->append(&_numDirtyEviction);
	_numLoadHit.init("loadHit", "Load Hit");
	memStats->append(&_numLoadHit);
	_numLoadMiss.init("loadMiss", "Load Miss");
	memStats->append(&_numLoadMiss);
	_numStoreHit.init("storeHit", "Store Hit");
	memStats->append(&_numStoreHit);
	_numStoreMiss.init("storeMiss", "Store Miss");
	memStats->append(&_numStoreMiss);
	_numCounterAccess.init("counterAccess", "Counter Access");
	memStats->append(&_numCounterAccess);

	_numTagLoad.init("tagLoad", "Number of tag loads");
	memStats->append(&_numTagLoad);
	_numTagStore.init("tagStore", "Number of tag stores");
	memStats->append(&_numTagStore);
	_numTagBufferFlush.init("tagBufferFlush", "Number of tag buffer flushes");
	memStats->append(&_numTagBufferFlush);

	_numTBDirtyHit.init("TBDirtyHit", "Tag buffer hits (LLC dirty evict)");
	memStats->append(&_numTBDirtyHit);
	_numTBDirtyMiss.init("TBDirtyMiss", "Tag buffer misses (LLC dirty evict)");
	memStats->append(&_numTBDirtyMiss);

	_numTouchedLines.init("totalTouchLines", "total # of touched lines in UnisonCache");
	memStats->append(&_numTouchedLines);
	_numEvictedLines.init("totalEvictLines", "total # of evicted lines in UnisonCache");
	memStats->append(&_numEvictedLines);

	_ext_dram->initStats(memStats);
	for (uint32_t i = 0; i < _mcdram_per_mc; i++)
		_mcdram[i]->initStats(memStats);

	parentStat->append(memStats);
}

Address
MemoryController::transMCAddress(Address mc_addr)
{
	// 28 lines per DRAM row (2048 KB row)
	uint64_t num_lines_per_mc = 128 * 1024 * 1024 / 2048 * 28;
	uint64_t set = mc_addr % num_lines_per_mc;
	return set / 28 * 32 + set % 28;
}

Address
MemoryController::transMCAddressPage(uint64_t set_num, uint32_t way_num)
{
	return (_num_ways * set_num + way_num) * _granularity;
}

TagBuffer::TagBuffer(Config &config)
{
	uint32_t tb_size = config.get<uint32_t>("sys.mem.mcdram.tag_buffer_size", 1024);
	_num_ways = 8;
	_num_sets = tb_size / _num_ways;
	_entry_occupied = 0;
	_tag_buffer = (TagBufferEntry **)gm_malloc(sizeof(TagBufferEntry *) * _num_sets);
	//_tag_buffer = new TagBufferEntry * [_num_sets];
	for (uint32_t i = 0; i < _num_sets; i++)
	{
		_tag_buffer[i] = (TagBufferEntry *)gm_malloc(sizeof(TagBufferEntry) * _num_ways);
		//_tag_buffer[i] = new TagBufferEntry [_num_ways];
		for (uint32_t j = 0; j < _num_ways; j++)
		{
			_tag_buffer[i][j].remap = false;
			_tag_buffer[i][j].tag = 0;
			_tag_buffer[i][j].lru = j;
		}
	}
}

uint32_t
TagBuffer::existInTB(Address tag)
{
	uint32_t set_num = tag % _num_sets;
	for (uint32_t i = 0; i < _num_ways; i++)
		if (_tag_buffer[set_num][i].tag == tag)
		{
			// printf("existInTB\n");
			return i;
		}
	return _num_ways;
}

bool TagBuffer::canInsert(Address tag)
{
#if 1
	uint32_t num = 0;
	for (uint32_t i = 0; i < _num_sets; i++)
		for (uint32_t j = 0; j < _num_ways; j++)
			if (_tag_buffer[i][j].remap)
				num++;
	assert(num == _entry_occupied);
#endif

	uint32_t set_num = tag % _num_sets;
	// printf("tag_buffer=%#lx, set_num=%d, tag_buffer[set_num]=%#lx, num_ways=%d\n",
	//	(uint64_t)_tag_buffer, set_num, (uint64_t)_tag_buffer[set_num], _num_ways);
	for (uint32_t i = 0; i < _num_ways; i++)
		if (!_tag_buffer[set_num][i].remap || _tag_buffer[set_num][i].tag == tag)
			return true;
	return false;
}

bool TagBuffer::canInsert(Address tag1, Address tag2)
{
	uint32_t set_num1 = tag1 % _num_sets;
	uint32_t set_num2 = tag2 % _num_sets;
	if (set_num1 != set_num2)
		return canInsert(tag1) && canInsert(tag2);
	else
	{
		uint32_t num = 0;
		for (uint32_t i = 0; i < _num_ways; i++)
			if (!_tag_buffer[set_num1][i].remap || _tag_buffer[set_num1][i].tag == tag1 || _tag_buffer[set_num1][i].tag == tag2)
				num++;
		return num >= 2;
	}
}

void TagBuffer::insert(Address tag, bool remap)
{
	uint32_t set_num = tag % _num_sets;
	uint32_t exist_way = existInTB(tag);
#if 1
	for (uint32_t i = 0; i < _num_ways; i++)
		for (uint32_t j = i + 1; j < _num_ways; j++)
		{
			// if (_tag_buffer[set_num][i].tag != 0 && _tag_buffer[set_num][i].tag == _tag_buffer[set_num][j].tag) {
			//	for (uint32_t k = 0; k < _num_ways; k++)
			//		printf("_tag_buffer[%d][%d]: tag=%ld, remap=%d\n",
			//			set_num, k, _tag_buffer[set_num][k].tag, _tag_buffer[set_num][k].remap);
			// }
			assert(_tag_buffer[set_num][i].tag != _tag_buffer[set_num][j].tag || _tag_buffer[set_num][i].tag == 0);
		}
#endif
	if (exist_way < _num_ways)
	{
		// the tag already exists in the Tag Buffer
		assert(tag == _tag_buffer[set_num][exist_way].tag);
		if (remap)
		{
			if (!_tag_buffer[set_num][exist_way].remap)
				_entry_occupied++;
			_tag_buffer[set_num][exist_way].remap = true;
		}
		else if (!_tag_buffer[set_num][exist_way].remap)
			updateLRU(set_num, exist_way);
		return;
	}

	uint32_t max_lru = 0;
	uint32_t replace_way = _num_ways;
	for (uint32_t i = 0; i < _num_ways; i++)
	{
		if (!_tag_buffer[set_num][i].remap && _tag_buffer[set_num][i].lru >= max_lru)
		{
			max_lru = _tag_buffer[set_num][i].lru;
			replace_way = i;
		}
	}
	assert(replace_way != _num_ways);
	_tag_buffer[set_num][replace_way].tag = tag;
	_tag_buffer[set_num][replace_way].remap = remap;
	if (!remap)
	{
		// printf("\tset=%d way=%d, insert. no remap\n", set_num, replace_way);
		updateLRU(set_num, replace_way);
	}
	else
	{
		// printf("set=%d way=%d, insert\n", set_num, replace_way);
		_entry_occupied++;
	}
}

void TagBuffer::updateLRU(uint32_t set_num, uint32_t way)
{
	assert(!_tag_buffer[set_num][way].remap);
	for (uint32_t i = 0; i < _num_ways; i++)
		if (!_tag_buffer[set_num][i].remap && _tag_buffer[set_num][i].lru < _tag_buffer[set_num][way].lru)
			_tag_buffer[set_num][i].lru++;
	_tag_buffer[set_num][way].lru = 0;
}

void TagBuffer::clearTagBuffer()
{
	_entry_occupied = 0;
	for (uint32_t i = 0; i < _num_sets; i++)
	{
		for (uint32_t j = 0; j < _num_ways; j++)
		{
			_tag_buffer[i][j].remap = false;
			_tag_buffer[i][j].tag = 0;
			_tag_buffer[i][j].lru = j;
		}
	}
}
