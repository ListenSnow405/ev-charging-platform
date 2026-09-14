# 大数据可视化大屏（Vue3 + DataV）

> T6 产出，归属 L5。渲染冒烟见 [../../scripts/smoke-screen-phase2.py](../../scripts/smoke-screen-phase2.py)。

## 技术栈实测版本

| 项 | 版本 |
| --- | --- |
| Node | v24.1.0（LTS；要求 23+，v23 是奇数线已 EOL） |
| Vue | 3.5.42 |
| Vite | 6.4.3 |
| ECharts | 5.6.0 |
| DataV | `@kjgl77/datav-vue3` 1.7.4 |

## 运行

```bash
# 1. 先起 Flask（大屏所有数据都来自它）
.venv-phase2/bin/python bigdata/api/app.py

# 2. 前端
export NVM_DIR="$HOME/.nvm" && . "$NVM_DIR/nvm.sh"
cd bigdata/web
npm run dev        # 开发，http://127.0.0.1:5173
npm run build && npm run preview   # 生产预览，http://127.0.0.1:4173

# 3. 渲染验证（会真的开无头浏览器截图并断言）
.venv-phase2/bin/python scripts/smoke-screen-phase2.py
```

`vite.config.js` 里配了 `/api` → `127.0.0.1:5000` 的代理，开发与预览都走同源，
所以前端代码里的路径在两种模式下一致，不必区分环境。

## 面板与维度对照

| 面板 | 维度 | 图表形态 |
| --- | --- | --- |
| 顶部 KPI × 6 | `/api/overview` | DataV **数字翻牌** |
| 站点营收排行 | D2 | DataV **胶囊图** |
| 订单终态构成 | D6 | DataV **活动环图** |
| 电桩在线率 | D7 | ECharts **仪表盘** |
| 电桩利用率 Top12 | D3 | **横向条形**（快慢充用色区分） |
| 营收与订单趋势 | D1 | **折线 + 渐变面积**，双轴 |
| 工作日 vs 周末 | C2 对比 | **双折线面积** |
| 站点地理分布 | D10 | **经纬度散点**，点径随营收 |
| 时段负荷分布 | D4 | **柱 + 折线**双轴 |
| 快充 vs 慢充 | C1 对比 | **分组柱**（对数轴） |
| 站点多指标对标 | C3 对比 | **雷达图** |
| 碳排放与峰平谷 | D9 | **堆叠柱 + 折线** |
| 充电时长分布 | D11 | **并列柱**（对数轴） |
| 充电量分桶 | D5 | **渐变柱** |
| 设备事件时序 | D8 | **散点**，点径随事件数 |

共 **14 个面板 / 9 类图表形态**，对应「图表格式不要过于单一」的要求。

## 几个不显然的处理，改代码前先看

1. **DataV 的样式要手动引入。** 它的 `package.json` 没有 `style` 字段，但
   `dist/style.css` 确实存在。不引的话边框与装饰组件全是裸的。见 `src/main.js`。
2. **胶囊图用「万元」不用「元」。** 它底部会画数值刻度轴，10 万级的数字会把刻度标签
   挤成一串连在一起的乱码（实测 `021139422786341784556105695`）。
3. **C1 与 D11 用对数轴。** C1 四个指标量纲从两位数跨到四位数，D11 慢充尾巴拖到 420
   分钟且每桶个位数——线性轴都会把小的一侧压成贴地的一条线。
4. **C3 雷达的取消率在服务端已取反**（`cancel_score` 越高 = 取消率越低），
   所以各轴统一「越大越好」，面积大 = 综合表现好。直接用 `cancel_rate_pct` 画会反着读。
5. **D10 开了 `labelLayout.hideOverlap`。** 六个站经纬度相近，标签必然互相压；
   显示不全好过糊成一团，完整信息在 tooltip。
6. **`EChart.vue` 用 `ResizeObserver` 而非 `window.resize`。** 大屏是网格布局，
   面板自身尺寸变化不一定触发 `window.resize`。销毁时必须 `dispose()`，否则长时间运行会泄漏。

## 渲染冒烟为什么不用 `firefox --headless --screenshot`

本机上它**退出码 0 却不产出文件**，第一阶段就踩过（见 `L5-PLAN.md` 第 4 节）。
改走 geckodriver 的 WebDriver REST 协议：纯 HTTP，不需要装 selenium，
除了截图还能对 DOM 下断言。

冒烟里有一条曾经写错的断言值得记：`DvDigitalFlop` 渲染进 `<canvas>`，
**没有任何文本节点**，`innerText` 和 `textContent` 都恒为空——
拿它们断言是永远误报失败的写法。现在改为统计 canvas 的非透明像素数。
