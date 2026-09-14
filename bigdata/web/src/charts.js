// ECharts 配置构建。
//
// 图表配置集中在此、不散进各组件，是为了让**配色与坐标轴样式只定义一次**——
// 大屏上十来张图如果各写各的，深浅不一会非常明显。
//
// 单位约定：入参一律是 API 原样返回的整数（分 / 度×100），
// 在这里换算成展示单位，组件层不再二次换算。

import { fenToYuan, kwhOf } from './api.js'

// 一套配色，按「主-辅-强调-警示」分工，不是随机取色
export const C = {
  primary: '#36cfc9',
  secondary: '#597ef7',
  accent: '#ffc53d',
  warn: '#ff7875',
  purple: '#9254de',
  text: '#c9d6e5',
  axis: 'rgba(120,160,200,.25)'
}

const SERIES_COLORS = [C.primary, C.secondary, C.accent, C.warn, C.purple, '#73d13d']

// 所有图共用的底子：透明背景、统一字号、贴边的 grid
const base = (extra = {}) => ({
  backgroundColor: 'transparent',
  color: SERIES_COLORS,
  textStyle: { color: C.text, fontSize: 11 },
  tooltip: {
    trigger: 'axis',
    backgroundColor: 'rgba(10,25,47,.92)',
    borderColor: C.primary,
    textStyle: { color: '#e6f7ff', fontSize: 11 }
  },
  grid: { left: 42, right: 16, top: 28, bottom: 24, containLabel: true },
  ...extra
})

const axis = (extra = {}) => ({
  axisLine: { lineStyle: { color: C.axis } },
  axisLabel: { color: C.text, fontSize: 10 },
  splitLine: { lineStyle: { color: C.axis, type: 'dashed' } },
  ...extra
})

/** D1 营收趋势：折线 + 渐变面积 */
export function d1RevenueTrend (rows) {
  return base({
    legend: { data: ['营收(元)', '订单数'], textStyle: { color: C.text }, top: 0, itemWidth: 14 },
    xAxis: { type: 'category', data: rows.map(r => String(r.order_date).slice(5)), ...axis({ splitLine: { show: false } }) },
    yAxis: [
      { type: 'value', name: '元', ...axis() },
      { type: 'value', name: '单', ...axis({ splitLine: { show: false } }) }
    ],
    series: [
      {
        name: '营收(元)', type: 'line', smooth: true, showSymbol: false,
        data: rows.map(r => +fenToYuan(r.revenue_fen).toFixed(2)),
        areaStyle: {
          color: {
            type: 'linear', x: 0, y: 0, x2: 0, y2: 1,
            colorStops: [
              { offset: 0, color: 'rgba(54,207,201,.45)' },
              { offset: 1, color: 'rgba(54,207,201,.02)' }
            ]
          }
        },
        lineStyle: { width: 2 }
      },
      {
        name: '订单数', type: 'line', yAxisIndex: 1, smooth: true, showSymbol: false,
        data: rows.map(r => r.order_cnt), lineStyle: { width: 1.5, type: 'dashed' }
      }
    ]
  })
}

/** D3 电桩利用率：横向条形，只取前 12 根，再多会挤成一团 */
export function d3PileUtilization (rows) {
  const top = rows.slice(0, 12).reverse()
  return base({
    grid: { left: 8, right: 46, top: 10, bottom: 8, containLabel: true },
    tooltip: { trigger: 'axis', axisPointer: { type: 'shadow' } },
    xAxis: { type: 'value', ...axis() },
    yAxis: { type: 'category', data: top.map(r => r.pile_code), ...axis({ splitLine: { show: false } }) },
    series: [{
      type: 'bar', barWidth: '58%',
      data: top.map(r => ({
        value: r.utilization_pct,
        // 快慢充用颜色区分，省一条图例
        itemStyle: { color: r.type_label === '快充' ? C.primary : C.accent }
      })),
      label: { show: true, position: 'right', color: C.text, fontSize: 10, formatter: '{c}%' }
    }]
  })
}

/** D4 时段负荷：柱 + 折线双轴 */
export function d4HourlyLoad (rows) {
  return base({
    legend: { data: ['订单数', '电量(度)'], textStyle: { color: C.text }, top: 0, itemWidth: 14 },
    xAxis: { type: 'category', data: rows.map(r => r.order_hour + '时'), ...axis({ splitLine: { show: false } }) },
    yAxis: [{ type: 'value', ...axis() }, { type: 'value', ...axis({ splitLine: { show: false } }) }],
    series: [
      { name: '订单数', type: 'bar', barWidth: '52%', data: rows.map(r => r.order_cnt), itemStyle: { color: C.secondary, borderRadius: [3, 3, 0, 0] } },
      { name: '电量(度)', type: 'line', yAxisIndex: 1, smooth: true, symbolSize: 4, data: rows.map(r => +kwhOf(r.kwh_x100).toFixed(1)), lineStyle: { color: C.accent, width: 2 } }
    ]
  })
}

/** D5 充电量分桶 */
export function d5KwhDistribution (rows) {
  return base({
    tooltip: { trigger: 'axis', axisPointer: { type: 'shadow' } },
    xAxis: { type: 'category', data: rows.map(r => `${r.kwh_from}-${r.kwh_to}`), name: '度', ...axis({ splitLine: { show: false } }) },
    yAxis: { type: 'value', ...axis() },
    series: [{
      type: 'bar', barWidth: '56%', data: rows.map(r => r.order_cnt),
      itemStyle: {
        borderRadius: [4, 4, 0, 0],
        color: { type: 'linear', x: 0, y: 0, x2: 0, y2: 1, colorStops: [{ offset: 0, color: C.purple }, { offset: 1, color: 'rgba(146,84,222,.25)' }] }
      }
    }]
  })
}

/** D7 在线率仪表盘 */
export function d7OnlineGauge (rows) {
  const total = rows.reduce((s, r) => s + r.pile_cnt, 0)
  const online = rows.reduce((s, r) => s + r.online_cnt, 0)
  const pct = total ? +(online / total * 100).toFixed(1) : 0
  return {
    backgroundColor: 'transparent',
    series: [{
      type: 'gauge', startAngle: 210, endAngle: -30, min: 0, max: 100,
      radius: '92%', center: ['50%', '58%'],
      progress: { show: true, width: 10, itemStyle: { color: pct >= 90 ? C.primary : C.warn } },
      axisLine: { lineStyle: { width: 10, color: [[1, 'rgba(120,160,200,.18)']] } },
      axisTick: { show: false },
      splitLine: { length: 8, lineStyle: { color: C.axis, width: 1 } },
      axisLabel: { color: C.text, fontSize: 9, distance: 10 },
      pointer: { width: 4, itemStyle: { color: C.accent } },
      anchor: { show: true, size: 8, itemStyle: { color: C.accent } },
      detail: { valueAnimation: true, fontSize: 20, color: C.primary, offsetCenter: [0, '38%'], formatter: '{value}%' },
      title: { show: true, offsetCenter: [0, '68%'], color: C.text, fontSize: 10 },
      data: [{ value: pct, name: `在线 ${online}/${total}` }]
    }]
  }
}

/** D8 设备事件时序：散点，点大小随事件数 */
export function d8DeviceEvents (rows) {
  const kinds = [...new Set(rows.map(r => r.event_label))]
  return base({
    tooltip: { trigger: 'item', formatter: p => `${p.value[0]}<br/>${p.seriesName} ${p.value[2]} 次` },
    legend: { data: kinds, textStyle: { color: C.text }, top: 0, itemWidth: 12 },
    xAxis: { type: 'category', data: [...new Set(rows.map(r => String(r.event_date).slice(5)))], ...axis({ splitLine: { show: false } }) },
    yAxis: { type: 'category', data: kinds, ...axis() },
    series: kinds.map((k, i) => ({
      name: k, type: 'scatter',
      symbolSize: v => Math.min(8 + v[2] * 3, 26),
      itemStyle: { color: SERIES_COLORS[i % SERIES_COLORS.length], opacity: 0.85 },
      data: rows.filter(r => r.event_label === k)
        .map(r => [String(r.event_date).slice(5), k, r.event_cnt])
    }))
  })
}

/** D9 碳排放：峰平谷堆叠柱 + 排放折线 */
export function d9Carbon (rows) {
  const d = rows.map(r => String(r.stat_date).slice(5))
  const stack = (key, name, color) => ({
    name, type: 'bar', stack: 'kwh', barWidth: '62%',
    data: rows.map(r => +kwhOf(r[key]).toFixed(1)),
    itemStyle: { color }
  })
  return base({
    // 图例靠右、grid 顶部留白：默认居中会压住左轴的「度」字
    legend: { data: ['峰', '平', '谷', '排放(kg)'], textStyle: { color: C.text }, top: 0, right: 8, itemWidth: 12 },
    grid: { left: 42, right: 44, top: 40, bottom: 24, containLabel: true },
    xAxis: { type: 'category', data: d, ...axis({ splitLine: { show: false } }) },
    yAxis: [{ type: 'value', name: '度', nameGap: 12, ...axis() }, { type: 'value', name: 'kg', nameGap: 12, ...axis({ splitLine: { show: false } }) }],
    series: [
      stack('peak_kwh_x100', '峰', C.warn),
      stack('flat_kwh_x100', '平', C.secondary),
      stack('valley_kwh_x100', '谷', C.primary),
      {
        name: '排放(kg)', type: 'line', yAxisIndex: 1, smooth: true, showSymbol: false,
        data: rows.map(r => +(r.emission_g / 1000).toFixed(1)),
        lineStyle: { color: C.accent, width: 2 }
      }
    ]
  })
}

/** D10 站点地理分布：经纬度散点，点大小随营收 */
export function d10StationGeo (rows) {
  const max = Math.max(...rows.map(r => r.revenue_fen), 1)
  return base({
    tooltip: {
      trigger: 'item',
      formatter: p => `${p.data.name}<br/>营收 ${fenToYuan(p.data.rev).toFixed(2)} 元<br/>订单 ${p.data.cnt}`
    },
    grid: { left: 40, right: 24, top: 20, bottom: 24, containLabel: true },
    // 六个站经纬度差异只在小数点后两位，默认会打出 113.9135 这类长标签并连成一片。
    // 固定两位小数 + 限制刻度数量。
    xAxis: { type: 'value', name: '经度', scale: true, splitNumber: 4, ...axis({ axisLabel: { color: C.text, fontSize: 9, formatter: v => v.toFixed(2) } }) },
    yAxis: { type: 'value', name: '纬度', scale: true, splitNumber: 4, ...axis({ axisLabel: { color: C.text, fontSize: 9, formatter: v => v.toFixed(2) } }) },
    series: [{
      type: 'scatter',
      symbolSize: d => 14 + (d.rev / max) * 30,
      data: rows.map(r => ({
        value: [r.lng, r.lat], name: r.station_name, rev: r.revenue_fen, cnt: r.order_cnt
      })),
      itemStyle: {
        color: { type: 'radial', x: 0.5, y: 0.5, r: 0.5, colorStops: [{ offset: 0, color: 'rgba(54,207,201,.95)' }, { offset: 1, color: 'rgba(89,126,247,.35)' }] }
      },
      label: { show: true, formatter: p => p.data.name.replace('充电站', ''), position: 'right', distance: 6, color: C.text, fontSize: 9 },
      // 六个站经纬度相近，标签必然互相压。hideOverlap 让 ECharts 自动丢弃压住的那些，
      // 显示不全好过糊成一团——完整名单在 tooltip 里
      labelLayout: { hideOverlap: true }
    }]
  })
}

/** D11 充电时长分布：快慢充堆叠 */
export function d11Duration (rows) {
  const buckets = [...new Set(rows.map(r => r.minutes_from))].sort((a, b) => a - b)
  const of = label => buckets.map(b => {
    const hit = rows.find(r => r.minutes_from === b && r.type_label === label)
    return hit ? hit.order_cnt : 0
  })
  return base({
    legend: { data: ['快充', '慢充'], textStyle: { color: C.text }, top: 0, itemWidth: 12 },
    tooltip: {
      trigger: 'axis', axisPointer: { type: 'shadow' },
      formatter: ps => {
        const b = +ps[0].axisValue
        return `${b}-${b + 30} 分钟<br/>` +
          ps.filter(p => p.value != null).map(p => `${p.marker}${p.seriesName} ${p.value} 单`).join('<br/>')
      }
    },
    xAxis: {
      type: 'category', data: buckets.map(b => String(b)), name: '分钟',
      // 「0-30」这种区间标签两两相压。只标区间**起点**并旋转 45°，
      // 宽度立刻减半；完整区间在 tooltip 里能看到
      ...axis({ splitLine: { show: false }, axisLabel: { color: C.text, fontSize: 9, rotate: 45, interval: 0 } })
    },
    // 慢充尾巴拖到 420 分钟且每桶只有个位数，线性轴会把它们压成贴地的一条线，
    // 只看得见 0-30 那根。对数轴才能同时看见两个量级。
    // 堆叠 + 对数在 ECharts 里语义不清晰，所以这里**不堆叠**，改并列。
    yAxis: { type: 'log', min: 1, ...axis() },
    series: [
      { name: '快充', type: 'bar', data: of('快充').map(v => v || null), itemStyle: { color: C.primary } },
      { name: '慢充', type: 'bar', data: of('慢充').map(v => v || null), itemStyle: { color: C.accent } }
    ]
  })
}

/** C1 快充 vs 慢充：多指标分组柱。
 *  各指标量纲差得远（单均价上万分、时长几百分钟），所以**按指标分组而非同轴堆叠**，
 *  并各自标注单位，避免读者把两根柱子直接比高矮。 */
export function c1FastVsSlow (rows) {
  const pick = l => rows.find(r => r.type_label === l) || {}
  const fast = pick('快充'); const slow = pick('慢充')
  const items = [
    { name: '订单数(单)', f: fast.order_cnt, s: slow.order_cnt },
    { name: '单均(元)', f: +fenToYuan(fast.avg_amount_fen).toFixed(1), s: +fenToYuan(slow.avg_amount_fen).toFixed(1) },
    { name: '均时长(分)', f: fast.avg_minutes, s: slow.avg_minutes },
    { name: '取消率(%)', f: fast.cancel_rate_pct, s: slow.cancel_rate_pct }
  ]
  return base({
    legend: { data: ['快充', '慢充'], textStyle: { color: C.text }, top: 0, itemWidth: 12 },
    tooltip: { trigger: 'axis', axisPointer: { type: 'shadow' } },
    xAxis: { type: 'category', data: items.map(i => i.name), ...axis({ splitLine: { show: false }, axisLabel: { color: C.text, fontSize: 9, interval: 0 } }) },
    // 量纲跨度极大（订单数上千 vs 取消率两位数），线性轴会把小指标压成一条线。
    // 对数轴要求全部取值 > 0——这四个指标恒为正，成立。
    yAxis: { type: 'log', ...axis() },
    series: [
      { name: '快充', type: 'bar', barWidth: '30%', data: items.map(i => i.f), itemStyle: { color: C.primary, borderRadius: [3, 3, 0, 0] }, label: { show: true, position: 'top', color: C.text, fontSize: 9 } },
      { name: '慢充', type: 'bar', barWidth: '30%', data: items.map(i => i.s), itemStyle: { color: C.accent, borderRadius: [3, 3, 0, 0] }, label: { show: true, position: 'top', color: C.text, fontSize: 9 } }
    ]
  })
}

/** C2 工作日 vs 周末：双折线对比 24 小时形态。
 *  用**日均单量**而非总量——工作日 44 天、周末 16 天，直接比总量必然是工作日赢，
 *  那是天数差异不是行为差异。 */
export function c2WeekdayHourly (rows) {
  const hours = [...new Set(rows.map(r => r.order_hour))].sort((a, b) => a - b)
  const of = flag => hours.map(h => {
    const hit = rows.find(r => r.order_hour === h && !!r.is_weekend === flag)
    return hit ? hit.orders_per_day : 0
  })
  return base({
    legend: { data: ['工作日', '周末'], textStyle: { color: C.text }, top: 0, itemWidth: 14 },
    xAxis: { type: 'category', boundaryGap: false, data: hours.map(h => h + '时'), ...axis({ splitLine: { show: false } }) },
    yAxis: { type: 'value', name: '日均单', ...axis() },
    series: [
      { name: '工作日', type: 'line', smooth: true, showSymbol: false, data: of(false), lineStyle: { width: 2, color: C.secondary }, areaStyle: { color: 'rgba(89,126,247,.18)' } },
      { name: '周末', type: 'line', smooth: true, showSymbol: false, data: of(true), lineStyle: { width: 2, color: C.accent }, areaStyle: { color: 'rgba(255,197,61,.16)' } }
    ]
  })
}

/** C3 站点雷达。取消率在服务端已取反（cancel_score 越高越好），
 *  所以这里各轴都是「越大越好」，面积大 = 综合表现好，读图不会反。 */
export function c3StationRadar (rows) {
  return {
    backgroundColor: 'transparent',
    color: SERIES_COLORS,
    tooltip: { trigger: 'item', backgroundColor: 'rgba(10,25,47,.92)', borderColor: C.primary, textStyle: { color: '#e6f7ff', fontSize: 11 } },
    legend: { data: rows.map(r => r.station_name.replace('充电站', '')), textStyle: { color: C.text, fontSize: 9 }, bottom: 0, itemWidth: 10, itemHeight: 6 },
    radar: {
      indicator: [
        { name: '营收', max: 100 }, { name: '单量', max: 100 },
        { name: '时长', max: 100 }, { name: '单价', max: 100 },
        { name: '低取消', max: 100 }
      ],
      radius: '58%', center: ['50%', '46%'],
      axisName: { color: C.text, fontSize: 10 },
      splitLine: { lineStyle: { color: C.axis } },
      splitArea: { areaStyle: { color: ['rgba(20,40,70,.25)', 'rgba(20,40,70,.05)'] } },
      axisLine: { lineStyle: { color: C.axis } }
    },
    series: [{
      type: 'radar', symbolSize: 3,
      data: rows.map(r => ({
        name: r.station_name.replace('充电站', ''),
        value: [r.revenue_fen_score, r.order_cnt_score, r.charge_minutes_score, r.avg_unit_price_fen_score, r.cancel_score],
        lineStyle: { width: 1.5 },
        areaStyle: { opacity: 0.12 }
      }))
    }]
  }
}
