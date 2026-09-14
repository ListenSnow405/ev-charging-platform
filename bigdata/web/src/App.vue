<script setup>
import { computed, onMounted, onBeforeUnmount, reactive, ref } from 'vue'
import EChart from './components/EChart.vue'
import Panel from './components/Panel.vue'
import DataTable from './components/DataTable.vue'
import { fetchOverview, fetchDimension, fenToYuan, kwhOf, wan } from './api.js'
import { PAGES, ALL_DIMS } from './pages.js'
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
const dim = reactive({})

// ---- 分页 ----
const pageIdx = ref(0)
const page = computed(() => PAGES[pageIdx.value])
const autoPlay = ref(false)          // 答辩可开自动轮播；默认关闭，免得检查时页面乱跳
let rotator = null

function goto (i) {
  pageIdx.value = (i + PAGES.length) % PAGES.length
}
function toggleAuto () {
  autoPlay.value = !autoPlay.value
  if (rotator) { clearInterval(rotator); rotator = null }
  if (autoPlay.value) rotator = setInterval(() => goto(pageIdx.value + 1), 15000)
}
// 方向键翻页：答辩用的遥控笔多数映射为方向键，可以不碰鼠标
function onKey (e) {
  if (e.key === 'ArrowRight') goto(pageIdx.value + 1)
  else if (e.key === 'ArrowLeft') goto(pageIdx.value - 1)
  else if (e.key >= '1' && e.key <= String(PAGES.length)) goto(+e.key - 1)
}

async function load () {
  state.loading = true
  state.error = ''
  try {
    // 一次性取全部维度：翻页时不再发请求，切换是瞬时的。
    // 用 allSettled 而非 all：**单个维度缺失不应该让整屏变成错误页**。
    // 譬如 MLlib 还没跑完时 d12 表不存在，那一格显示「暂无数据」即可。
    const rs = await Promise.allSettled([
      fetchOverview(),
      ...ALL_DIMS.map(n => fetchDimension(n))
    ])
    if (rs[0].status === 'fulfilled') {
      overview.value = rs[0].value
    } else {
      // overview 挂了才是真的没数据——它是所有 KPI 的来源
      state.error = String(rs[0].reason?.message || rs[0].reason)
    }
    const missing = []
    ALL_DIMS.forEach((n, i) => {
      const r = rs[i + 1]
      if (r.status === 'fulfilled') dim[n] = r.value
      else { dim[n] = []; missing.push(n) }
    })
    // 缺哪几个要能看见，否则「暂无数据」和「忘了接线」分不出来
    if (missing.length) console.warn('[大屏] 以下维度取数失败：', missing.join(', '))
  } catch (e) {
    state.error = String(e.message || e)
  } finally {
    state.loading = false
  }
}

onMounted(() => {
  fit()
  window.addEventListener('resize', fit)
  window.addEventListener('keydown', onKey)
  clock = setInterval(() => {
    now.value = new Date().toLocaleString('zh-CN', { hour12: false })
  }, 1000)
  load()
})
onBeforeUnmount(() => {
  window.removeEventListener('resize', fit)
  window.removeEventListener('keydown', onKey)
  if (clock) clearInterval(clock)
  if (rotator) clearInterval(rotator)
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

const ringCfg = computed(() => ({
  data: (dim.d6_order_status || []).map(r => ({
    name: r.status_label, value: r.order_cnt
  })),
  lineWidth: 26,
  radius: '60%',
  activeRadius: '66%',
  digitalFlopStyle: { fill: '#e6f7ff', fontSize: 20 },
  color: ['#36cfc9', '#ff7875']
}))

/** 表格里的条形背景单元格：数值 + 视觉长度。
 *
 *  DvScrollBoard 的单元格支持 HTML，所以用一个绝对定位的色条垫在数字底下。
 *  `width` 按同列最大值归一——按满量程归一会让差异被压平。
 *  条形只是**辅助**，数值仍然完整显示，不能只靠长度读数。
 */
function bar (value, max, unit = '', color = '#36cfc9') {
  const pct = Math.max(2, Math.min(100, (value / max) * 100))
  // 用 flex + height:100% 撑满单元格并垂直居中。
  // 写死 height:18px 会让色条挂在行顶、串出行边界（第一版就是这个毛病）——
  // DvScrollBoard 的行高是按 rowNum 算出来的，不能假设。
  return `<div style="position:relative;display:flex;align-items:center;height:100%;">
    <div style="position:absolute;left:0;top:50%;transform:translateY(-50%);
                height:60%;width:${pct}%;
                background:linear-gradient(90deg,${color}55,${color}18);
                border-left:2px solid ${color};border-radius:2px;"></div>
    <span style="position:relative;padding-left:8px;">${value}${unit}</span>
  </div>`
}

// ---- 表格配置。键名对应 pages.js 里的 panel.table ----
//  数值在这里格式化成字符串，组件层不再换算——与 charts.js 同一口径。
const tables = {
  carbonStation: computed(() => {
    const rows = dim.d13_carbon_station || []
    // 条形长度按**本列最大值**归一，而不是按 100%——六个站都在 38~52% 之间，
    // 按 100% 归一的话所有条子都只有半截，差异根本看不出来。
    const maxPeak = Math.max(...rows.map(r => r.peak_pct), 1)
    return {
      header: ['站点', '排放(吨)', '电量(万度)', '强度 g/度', '订单', '峰占比'],
      columnWidth: [46, 96],
      align: ['center', 'left', 'right', 'right', 'right', 'right', 'left'],
      rowNum: 6,
      rows: rows.map(r => ([
        r.station_name.replace('充电站', ''),
        (r.emission_kg / 1000).toFixed(2),
        (r.kwh / 10000).toFixed(2),
        String(r.intensity_g_per_kwh),
        String(r.order_cnt),
        bar(r.peak_pct, maxPeak, '%')
      ]))
    }
  }),
  forecast: computed(() => {
    const rows = dim.d12_load_forecast || []
    const maxKw = Math.max(...rows.map(r => r.load_kw), 1)
    return {
      header: ['站点', 'h', '负荷 kW', '空闲桩', '状态'],
      columnWidth: [40, 84, 34],
      align: ['center', 'left', 'center', 'left', 'center', 'center'],
      rowNum: 9,
      rows: rows.map(r => ([
        r.station_name.replace('充电站', ''),
        r.horizon + 'h',
        bar(r.load_kw, maxKw),
        `${r.idle_pile}/${r.pile_total}`,
        // 高峰标红：这是运营真正要看的一列
        r.is_peak
          ? '<span style="color:#ff7875;font-weight:600">高峰</span>'
          : '<span style="color:#6d89a6">平峰</span>'
      ]))
    }
  }),
  carbonRecent: computed(() => ({
    header: ['日期', '电量(度)', '排放(kg)', '强度 g/度', '订单数', '完整度', '排放因子版本'],
    columnWidth: [46],
    align: ['center', 'center', 'right', 'right', 'right', 'right', 'center', 'center'],
    rowNum: 8,
    rows: (dim.d14_carbon_recent || []).map(r => ([
      String(r.stat_date),
      r.kwh.toFixed(1),
      r.emission_kg.toFixed(1),
      String(r.intensity_g_per_kwh),
      String(r.order_cnt),
      // 完整度低于 100% 要显眼——它直接影响排放量可不可信
      r.completeness >= 100
        ? '<span style="color:#73d13d">100%</span>'
        : `<span style="color:#ffc53d">${r.completeness}%</span>`,
      r.factor_version
    ]))
  }))
}

// 键名对应 pages.js 里的 panel.opt
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
  d12: () => B.d12Forecast(dim.d12_load_forecast),
  c1: () => B.c1FastVsSlow(dim.c1_fast_vs_slow),
  c2: () => B.c2WeekdayHourly(dim.c2_weekday_weekend_hourly),
  c3: () => B.c3StationRadar(dim.c3_station_radar)
}
</script>

<template>
  <div class="screen-wrap">
    <div class="screen" :style="{ transform: `scale(${scale})` }">
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

      <section class="kpis">
        <DvBorderBox12 v-for="k in kpis" :key="k.label" class="kpi">
          <div class="kpi-in">
            <DvDigitalFlop :config="flop(k.v, k.color)" class="kpi-num" />
            <div class="kpi-label">{{ k.label }}<span v-if="k.suffix" class="kpi-suffix">{{ k.suffix }}</span></div>
          </div>
        </DvBorderBox12>
      </section>

      <nav class="tabs">
        <button v-for="(p, i) in PAGES" :key="p.key"
                :class="['tab', { on: i === pageIdx }]" @click="goto(i)">
          <span class="tab-no">{{ i + 1 }}</span>{{ p.name }}
        </button>
        <span class="tab-desc">{{ page.desc }}</span>
        <button class="tab auto" :class="{ on: autoPlay }" @click="toggleAuto">
          {{ autoPlay ? '⏸ 停止轮播' : '▶ 自动轮播' }}
        </button>
        <span class="tab-hint">← → 翻页　数字键直达</span>
      </nav>

      <div v-if="state.error" class="global-err">
        <p>数据加载失败：{{ state.error }}</p>
        <p class="hint">请确认 Flask 已启动：<code>.venv-phase2/bin/python bigdata/api/app.py</code></p>
        <button @click="load">重试</button>
      </div>

      <!-- 当前页。用 :key 让翻页时图表重新 init——
           若改用 v-show，隐藏页的图表会以 0 尺寸初始化，切过去是错的 -->
      <main v-else :key="page.key" class="grid">
        <Panel v-for="p in page.panels" :key="p.tag + '/' + p.dim"
               class="g" :style="{ gridColumn: `span ${p.span}` }"
               :title="p.title" :tag="p.tag"
               :loading="state.loading" :empty="!has(p.dim)">
          <DvCapsuleChart v-if="p.kind === 'capsule'" :config="capsuleCfg" style="width:100%;height:100%" />
          <DvActiveRingChart v-else-if="p.kind === 'ring'" :config="ringCfg" style="width:100%;height:100%" />
          <DataTable v-else-if="p.kind === 'table'" v-bind="tables[p.table].value" />
          <EChart v-else :option="opt[p.opt]()" />
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

.kpis { display: grid; grid-template-columns: repeat(6, 1fr); gap: 12px; height: 92px; margin: 4px 0 8px; }
.kpi { height: 92px; }
.kpi-in { height: 100%; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 2px; }
.kpi-num { width: 100%; height: 36px; }
.kpi-label { font-size: 12px; color: #8fa9c4; letter-spacing: 1px; }
.kpi-suffix { color: #5c7a99; margin-left: 4px; }

.tabs { display: flex; align-items: center; gap: 10px; height: 40px; flex: 0 0 auto; margin-bottom: 8px; }
.tab { background: rgba(20, 45, 80, .45); border: 1px solid rgba(89, 126, 247, .35);
  color: #8fa9c4; padding: 6px 18px; border-radius: 4px; cursor: pointer;
  font-size: 14px; letter-spacing: 1px; font-family: inherit; transition: all .15s; }
.tab:hover { border-color: #36cfc9; color: #c9d6e5; }
.tab.on { background: rgba(54, 207, 201, .16); border-color: #36cfc9; color: #36cfc9; font-weight: 600; }
.tab-no { display: inline-block; width: 16px; height: 16px; line-height: 16px; margin-right: 7px;
  border-radius: 50%; background: rgba(255, 255, 255, .1); font-size: 10px; text-align: center; }
.tab.on .tab-no { background: #36cfc9; color: #04121f; }
.tab-desc { flex: 1 1 auto; color: #6d89a6; font-size: 13px; letter-spacing: .5px; padding-left: 6px; }
.tab.auto { padding: 6px 14px; font-size: 13px; }
.tab-hint { color: #4d6785; font-size: 11px; }

.grid { flex: 1 1 auto; min-height: 0; display: grid; gap: 14px;
  grid-template-columns: repeat(6, 1fr); grid-auto-rows: 1fr; }
.g { min-height: 0; }

.global-err { flex: 1 1 auto; display: flex; flex-direction: column; align-items: center;
  justify-content: center; gap: 12px; color: #ff7875; font-size: 16px; }
.global-err .hint { color: #7f9cb8; font-size: 13px; }
.global-err code { background: rgba(54,207,201,.12); padding: 2px 8px; border-radius: 3px; color: #36cfc9; }
.global-err button { background: transparent; border: 1px solid #36cfc9; color: #36cfc9;
  padding: 6px 22px; border-radius: 4px; cursor: pointer; font-size: 14px; }
.global-err button:hover { background: rgba(54,207,201,.12); }
</style>
