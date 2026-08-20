# SC3336 设备树核验（阶段 2）

本阶段未生成 DTS patch。原因不是跳过检查，而是板端基线已经证明当前运行配置与 SDK Ultra SC3336 定义一致；在没有反证时修改 GPIO、MCLK、I2C、regulator 或 endpoint 会引入不必要风险。

## 源码依据

- DTS 入口：`sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-luckfox-pico-ultra.dts`
- SC3336 节点：`sysdrv/source/kernel/arch/arm/boot/dts/rv1106-luckfox-pico-ultra-ipc.dtsi`
- 节点：`&i2c4 { sc3336: sc3336@30 { ... } }`
- compatible：`smartsens,sc3336`
- I2C 地址：`0x30`
- endpoint：`sc3336_out`，data lanes 为 `<1 2>`

## 板端核验结果

板端 `media-ctl -p` 显示：

```text
m00_b_sc3336 4-0030 (/dev/v4l-subdev2)
  -> rockchip-csi2-dphy0 (/dev/v4l-subdev1)
  -> rockchip-mipi-csi2
  -> rkcif-mipi-lvds (/dev/media0)
  -> rkisp-vir0 (/dev/media1)
  -> rkisp_mainpath (/dev/video11)
```

传感器与 DPHY 链路处于 enabled 状态，格式为 `SBGGR10_1X10/2304x1296`；`/dev/video11` 成功采到 2304x1296 NV12 单帧。

## 结论与限制

- 普通 Ultra 启动链路已验证为 SC3336，不能套用 `rv1106g-luckfox-pico-ultra-fastboot.dts` 中的 SC3338 配置。
- 本阶段不添加或变更 GPIO、regulator、MCLK、endpoint、I2C 地址或 mode。
- 若后续更换 Ultra W、camera 模组或启动 DTB，必须重新采集 baseline 后再评估 DTS patch。

