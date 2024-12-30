# Zsim-Hybrid2 [Current Version 1.0.1]

This project aims to reproduce the **HPCA'2020** paper `"Hybrid2: Combining Caching and Migration in Hybrid Memory Systems"` using ZSim. The project is implemented based on the open-source Banshee project. 

The link to the open-source Banshee project is `https://github.com/yxymit/banshee`.

## v1.0.0
The first version of Zsim-Hybrid2 is released. The project is implemented based on the open-source Banshee project.

## v1.0.1
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
