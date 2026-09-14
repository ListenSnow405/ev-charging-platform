<script setup>
import { computed, onMounted, onBeforeUnmount, reactive, ref } from 'vue'
import EChart from './components/EChart.vue'
import Panel from './components/Panel.vue'
import { fetchOverview, fetchDimension, fenToYuan, kwhOf, wan } from './api.js'
import * as B from './charts.js'

// 设计稿 1920×1080，实际按比例缩放。投影分辨率不可控，
// 等比缩放比响应式断点可靠——后者会在非常规比例下把图挤变形。
const DESIGN_W = 1920
const DESIGN_H = 1080
const scale = ref(1)
function fit () {
  scale.value = Math.min(window.innerWidth / DESIGN_W, window.innerHeight / DESIGN_H)
}

const now = ref('')
let clock = null

const state = reactive({ loading: true, error: '' })
const overview = ref(null)
const dim = reactive({})          // 维度名 → rows

// 面板与维度的对应关系集中在这里，便于对照 PHASE2-PLAN 第 7 节
const NEEDED = [
  'd1_revenue_trend', 'd2_station_rank', 'd3_pile_utilization', 'd4_hourly_load',
  'd5_kwh_distribution', 'd6_order_status', 'd7_pile_status', 'd8_device_events',
  'd9_carbon_daily', 'd10_station_geo', 'd11_duration_distribution',
  'c1_fast_vs_slow', 'c2_weekday_weekend_hourly', 'c3_station_radar'
]

async function load () {
  state.loading = true
  state.error = ''
  try {
    // 并发取数：十几个小请求串行会让首屏明显变慢
    const [ov, ...rest] = await Promise.all([
      fetchOverview(),
      ...NEEDED.map(n => fetchDimension(n))
    ])
    overview.value = ov
    NEEDED.forEach((n, i) => { dim[n] = rest[i] })
  } catch (e) {
    // 把真实错误显示出来而不是吞掉——大屏出问题时，
    // 「接口 404」和「数据库没起来」需要现场就能分辨
    state.error = String(e.message || e)
  } finally {
    state.loading = false
  }
}

onMounted(() => {
  fit()
  window.addEventListener('resize', fit)
  clock = setInterval(() => {
    now.value = new Date().toLocaleString('zh-CN', { hour12: false })
  }, 1000)
  load()
})
onBeforeUnmount(() => {
  window.removeEventListener('resize', fit)
  if (clock) clearInterval(clock)
})

const has = n => Array.isArray(dim[n]) && dim[n].length > 0

// ---- KPI：DigitalFlop 需要 {number:[...], content:'{nt}'} 结构 ----
const kpis = computed(() => {
  const o = overview.value
  if (!o) return []
  return [
    { label: '累计营收（万元）', v: +wan(fenToYuan(o.revenue_fen)).toFixed(2), color: '#36cfc9' },
    { label: '累计电量（万度）', v: +wan(kwhOf(o.kwh_x100)).toFixed(2), color: '#597ef7' },
    { label: '订单总数（单）', v: o.order_total, color: '#ffc53d' },
    { label: '结算率（%）', v: o.settle_rate_pct, color: '#73d13d' },
    { label: '电桩在线', v: o.pile_online, suffix: `/ ${o.pile_total}`, color: '#9254de' },
    { label: '碳排放（吨）', v: +(o.emission_g / 1e6).toFixed(1), color: '#ff7875' }
  ]
})

const flop = (v, color) => ({
  number: [v],
  content: '{nt}',
  style: { fill: color, fontSize: 26, fontWeight: 'bold' },
  toFixed: Number.isInteger(v) ? 0 : (String(v).split('.')[1] || '').length
})

// ---- D2 站点排行用 DataV 的胶囊图，换个图表形态，别全是 ECharts ----
const capsuleCfg = computed(() => ({
  data: (dim.d2_station_rank || []).map(r => ({
    name: r.station_name.replace('充电站', ''),
    // 用万元而非元：胶囊图底部会画数值刻度轴，10 万级的数字会把刻度标签
    // 挤成一串连在一起的乱码（实测 "021139422786341784556105695"）
    value: +wan(fenToYuan(r.revenue_fen)).toFixed(1)
  })),
  unit: '万元',
  showValue: true,
  colors: ['#36cfc9', '#597ef7']
}))

// ---- D6 订单终态用 DataV 的活动环图 ----
const ringCfg = computed(() => ({
  data: (dim.d6_order_status || []).map(r => ({
    name: r.status_label, value: r.order_cnt
  })),
  lineWidth: 22,
  radius: '58%',
  activeRadius: '64%',
  digitalFlopStyle: { fill: '#e6f7ff', fontSize: 18 },
  color: ['#36cfc9', '#ff7875']
}))

const opt = {
  d1: () => B.d1RevenueTrend(dim.d1_revenue_trend),
  d3: () => B.d3PileUtilization(dim.d3_pile_utilization),
  d4: () => B.d4HourlyLoad(dim.d4_hourly_load),
  d5: () => B.d5KwhDistribution(dim.d5_kwh_distribution),
  d7: () => B.d7OnlineGauge(dim.d7_pile_status),
  d8: () => B.d8DeviceEvents(dim.d8_device_events),
  d9: () => B.d9Carbon(dim.d9_carbon_daily),
  d10: () => B.d10StationGeo(dim.d10_station_geo),
  d11: () => B.d11Duration(dim.d11_duration_distribution),
  c1: () => B.c1FastVsSlow(dim.c1_fast_vs_slow),
  c2: () => B.c2WeekdayHourly(dim.c2_weekday_weekend_hourly),
  c3: () => B.c3StationRadar(dim.c3_station_radar)
}
</script>

<template>
  <div class="screen-wrap">
    <div class="screen" :style="{ transform: `scale(${scale})` }">
      <!-- 顶栏 -->
      <header class="head">
        <DvDecoration10 class="deco-line" />
        <div class="head-mid">
          <DvDecoration8 :reverse="true" class="deco-8" />
          <h1>充 电 桩 运 营 大 数 据 分 析 平 台</h1>
          <DvDecoration8 class="deco-8" />
        </div>
        <DvDecoration10 class="deco-line flip" />
        <div class="clock">{{ now }}</div>
      </header>

      <!-- KPI -->
      <section class="kpis">
        <DvBorderBox12 v-for="k in kpis" :key="k.label" class="kpi">
          <div class="kpi-in">
            <DvDigitalFlop :config="flop(k.v, k.color)" class="kpi-num" />
            <div class="kpi-label">{{ k.label }}<span v-if="k.suffix" class="kpi-suffix">{{ k.suffix }}</span></div>
          </div>
        </DvBorderBox12>
      </section>

      <!-- 全局错误：接口全挂时给一条能照着排查的提示 -->
      <div v-if="state.error" class="global-err">
        <p>数据加载失败：{{ state.error }}</p>
        <p class="hint">请确认 Flask 已启动：<code>.venv-phase2/bin/python bigdata/api/app.py</code></p>
        <button @click="load">重试</button>
      </div>

      <!-- 主体三列 -->
      <main v-else class="grid">
        <!-- 左列 -->
        <Panel class="g" title="站点营收排行" tag="D2" :loading="state.loading" :empty="!has('d2_station_rank')">
          <DvCapsuleChart :config="capsuleCfg" style="width:100%;height:100%" />
        </Panel>
        <Panel class="g" title="订单终态构成" tag="D6" :loading="state.loading" :empty="!has('d6_order_status')">
          <DvActiveRingChart :config="ringCfg" style="width:100%;height:100%" />
        </Panel>
        <Panel class="g" title="电桩在线率" tag="D7" :loading="state.loading" :empty="!has('d7_pile_status')">
          <EChart :option="opt.d7()" />
        </Panel>
        <Panel class="g" title="电桩利用率 Top12" tag="D3" :loading="state.loading" :empty="!has('d3_pile_utilization')">
          <EChart :option="opt.d3()" />
        </Panel>

        <!-- 中列 -->
        <Panel class="g wide" title="营收与订单趋势（近 60 日）" tag="D1" :loading="state.loading" :empty="!has('d1_revenue_trend')">
          <EChart :option="opt.d1()" />
        </Panel>
        <Panel class="g wide" title="工作日 vs 周末 · 24 小时日均单量" tag="C2 对比" :loading="state.loading" :empty="!has('c2_weekday_weekend_hourly')">
          <EChart :option="opt.c2()" />
        </Panel>
        <Panel class="g" title="站点地理分布" tag="D10" :loading="state.loading" :empty="!has('d10_station_geo')">
          <EChart :option="opt.d10()" />
        </Panel>
        <Panel class="g" title="时段负荷分布" tag="D4" :loading="state.loading" :empty="!has('d4_hourly_load')">
          <EChart :option="opt.d4()" />
        </Panel>

        <!-- 右列 -->
        <Panel class="g" title="快充 vs 慢充" tag="C1 对比" :loading="state.loading" :empty="!has('c1_fast_vs_slow')">
          <EChart :option="opt.c1()" />
        </Panel>
        <Panel class="g" title="站点多指标对标" tag="C3 对比" :loading="state.loading" :empty="!has('c3_station_radar')">
          <EChart :option="opt.c3()" />
        </Panel>
        <Panel class="g" title="碳排放与峰平谷构成" tag="D9" :loading="state.loading" :empty="!has('d9_carbon_daily')">
          <EChart :option="opt.d9()" />
        </Panel>
        <Panel class="g" title="充电时长分布" tag="D11" :loading="state.loading" :empty="!has('d11_duration_distribution')">
          <EChart :option="opt.d11()" />
        </Panel>

        <!-- 底排 -->
        <Panel class="g wide" title="充电量分桶" tag="D5" :loading="state.loading" :empty="!has('d5_kwh_distribution')">
          <EChart :option="opt.d5()" />
        </Panel>
        <Panel class="g wide" title="设备事件时序" tag="D8" :loading="state.loading" :empty="!has('d8_device_events')">
          <EChart :option="opt.d8()" />
        </Panel>
      </main>
    </div>
  </div>
</template>

<style scoped>
.screen-wrap { width: 100vw; height: 100vh; display: flex; align-items: center; justify-content: center;
  background: radial-gradient(ellipse at 50% 0%, #0b1c33 0%, #050d1a 65%); overflow: hidden; }
.screen { width: 1920px; height: 1080px; transform-origin: center center; flex: 0 0 auto;
  display: flex; flex-direction: column; padding: 10px 18px 16px; }

.head { position: relative; height: 66px; display: flex; align-items: center; justify-content: center; gap: 14px; }
.head-mid { display: flex; align-items: center; gap: 16px; }
.head h1 { font-size: 30px; letter-spacing: 6px; font-weight: 700;
  background: linear-gradient(180deg, #ffffff 30%, #36cfc9 100%);
  -webkit-background-clip: text; background-clip: text; color: transparent; white-space: nowrap; }
.deco-line { width: 300px; height: 18px; }
.deco-line.flip { transform: rotateY(180deg); }
.deco-8 { width: 180px; height: 40px; }
.clock { position: absolute; right: 6px; bottom: 4px; font-size: 13px; color: #7f9cb8; letter-spacing: 1px; }

.kpis { display: grid; grid-template-columns: repeat(6, 1fr); gap: 12px; height: 92px; margin: 4px 0 10px; }
.kpi { height: 92px; }
.kpi-in { height: 100%; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 2px; }
.kpi-num { width: 100%; height: 36px; }
.kpi-label { font-size: 12px; color: #8fa9c4; letter-spacing: 1px; }
.kpi-suffix { color: #5c7a99; margin-left: 4px; }

.grid { flex: 1 1 auto; min-height: 0; display: grid; gap: 12px;
  grid-template-columns: repeat(6, 1fr); grid-auto-rows: 1fr; }
.g { min-height: 0; grid-column: span 1; }
.g.wide { grid-column: span 2; }

.global-err { flex: 1 1 auto; display: flex; flex-direction: column; align-items: center;
  justify-content: center; gap: 12px; color: #ff7875; font-size: 16px; }
.global-err .hint { color: #7f9cb8; font-size: 13px; }
.global-err code { background: rgba(54,207,201,.12); padding: 2px 8px; border-radius: 3px; color: #36cfc9; }
.global-err button { background: transparent; border: 1px solid #36cfc9; color: #36cfc9;
  padding: 6px 22px; border-radius: 4px; cursor: pointer; font-size: 14px; }
.global-err button:hover { background: rgba(54,207,201,.12); }
</style>
