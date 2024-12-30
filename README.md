# Zsim-Hybrid2 [Current Version 1.0.1]

This project aims to reproduce the **HPCA'2020** paper `"Hybrid2: Combining Caching and Migration in Hybrid Memory Systems"` using ZSim. The project is implemented based on the open-source Banshee project. 

The link to the open-source Banshee project is `https://github.com/yxymit/banshee`.

## v1.0.0
The first version of Zsim-Hybrid2 is released. The project is implemented based on the open-source Banshee project.

## v1.0.1
The second version of Zsim-Hybrid2 is released. The project is implemented based on the open-source Banshee project. The following changes are made:
1. Modified the logic of two remap tables, using page ids as keys and values.
2. The impact of traffic during the migration process is considered in this version, with `type == 2` requests used for load and store operations to represent migration and eviction.