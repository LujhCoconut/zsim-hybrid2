#!/bin/bash

# 设置 SPEC 根目录路径
# export SPEC=/home/dell/jhlu/spec_v4
# export PATH=$PATH:/usr/bin/:$SPEC/bin


# cd "$SPEC" || exit 1


# 执行 runcpu 命令
# echo "Running SPEC benchmark with runcpu..."
# /home/dell/jhlu/spec_v4/bin/runcpu --config=/home/dell/jhlu/spec_v4/config/myconfig_x86.cfg --action=run --tune=base --size=ref --noreportable 502.gcc_r

# 显示脚本完成的消息
# echo "Script execution completed."
chmod +x /home/dell/jhlu/spec_v4/benchspec/CPU/505.mcf_r/run/run_base_refrate_testx86-m64.0000/mcf_r_base.testx86-m64
cd /home/dell/jhlu/spec_v4/benchspec/CPU/505.mcf_r/run/run_base_refrate_testx86-m64.0000
pwd
bash mcf_r_base.testx86-m64