# Zsim-Hybrid2 [Current Version 2.0.0]

This project aims to reproduce the **HPCA'2020** paper `"Hybrid2: Combining Caching and Migration in Hybrid Memory Systems"` using ZSim. The project is implemented based on the open-source Banshee project. 

The link to the open-source Banshee project is `https://github.com/yxymit/banshee`.


## v2.0.0 (Current Version)
The 2.0.0 version of Zsim-Hybrid2 is released. Version 2.0.0 is an unstable release with significant room for performance optimization. The specific changes in version 2.0.0 are as follows:
1. Added the impact of asynchronous migration and eviction on system traffic and overhead, and fixed the "happens-before" issue introduced by asynchronous migration and eviction in the v1.0.x versions.
2. Fixed some bugs where certain states in the XTA were not set.

## v1.0.1 (Tested Simple Version)
The second version of Zsim-Hybrid2 is released. The project is implemented based on the open-source Banshee project. The following changes are made:
1. Modified the logic of two remap tables, using page ids as keys and values.
2. The impact of traffic during the migration process is considered in this version, with `type == 2` requests used for load and store operations to represent migration and eviction.

**IPC Test**

In the experimental testing of version v1.0.1, we used DDR-2400 as NM, HBM-2000 as FM, and Sys Frequency = 3400. Using deepsjeng from SPEC CPU2017 as the dataset, the experimental performance of hybrid-v1.0.1 was observed to be between that of pure DRAM and pure HBM.

```python
    if(tech == "HBM-2000"){
        tCK = 1;
        tBL = 4;
        tCL = uint32_t(18 / time_scale);
        tRCD = uint32_t( 12 / time_scale);
        tRTP = uint32_t( 5 / time_scale);
        tRP = uint32_t( 14 / time_scale);
        tRRD = uint32_t( 5 / time_scale); // 4+6/2
        tRAS = uint32_t( 28 / time_scale);
        tFAW = uint32_t( 15 / time_scale);
        tWTR = uint32_t( 4 / time_scale);// 4/12
        tWR = uint32_t( 14 / time_scale);
        tRFC = uint32_t( 220 / time_scale);
        tREFI = uint32_t(3900/ time_scale);
    }else if(tech == "DDR-2400"){
        tCK = 0.833;
        tBL = 8;
        tCL = uint32_t(14 / time_scale);
        tRCD = uint32_t( 14 / time_scale);
        tRTP = uint32_t( 8 / time_scale);
        tRP = uint32_t( 14 / time_scale);
        tRRD = uint32_t( 4 / time_scale); // 4+6/2
        tRAS = uint32_t( 32 / time_scale);
        tFAW = uint32_t( 15 / time_scale);
        tWTR = uint32_t( 5 / time_scale);// 4/12
        tWR = uint32_t( 15 / time_scale);
        tRFC = uint32_t( 350 / time_scale);
        tREFI = uint32_t(7800/ time_scale);
    }
```
In this experiment, the performance of the Hybrid2-v1.0.1 version improved by 13.38% compared to the pure DRAM baseline. However, it showed a 44.48% performance decrease compared to the pure HBM baseline. This indicates that there is room for optimization in the v1.0.1 code.

## v1.0.0
The first version of Zsim-Hybrid2 is released. The project is implemented based on the open-source Banshee project.

## How to build Zsim-Hybrid2
External dependencies: gcc >=4.6, pin, scons, libconfig, libhdf5, libelfg0

1. Clone the repository
```shell
git clone https://github.com/LujhCoconut/zsim-hybrid2.git
```

2. Download Pin 2.14 
[Pin 2.14](https://software.intel.com/sites/landingpage/pintool/downloads/pin-2.14-71313-gcc.4.4.7-linux.tar.gz). Tested with Pin 2.14 on an x86-64 architecture.

3. libconfig
[libconfig]( http://www.hyperrealm.com/libconfig). Tested with libconfig-1.7.3 on an x86-64 architecture.

4. libhdf5
[libhdf5](http://www.hdfgroup.org). v1.8.4 path 1 or higher.

5. compile
```shell
scons -j<You Want>
```

6. run
```shell
build/opt/zsim tests/<condfig_you_want.cfg>
```


