// 大屏分页配置。
//
// 15 个面板堆一屏的问题不是「放不下」，而是每格都太小——D3 的柱子被压扁、
// D11 的标签糊成一片。分页后每格面积约为原来的 3.5 倍。
//
// 分组按**业务逻辑**而非平均切块：一页回答一个问题，答辩时可以一页一句话带过。
// span 是在 6 列栅格中占的列数，每页合计应能被 6 整除，否则末行会缺口。

export const PAGES = [
  {
    key: 'ops',
    name: '运营总览',
    desc: '钱和单从哪来、什么时候来',
    panels: [
      { dim: 'd1_revenue_trend', title: '营收与订单趋势（近 60 日）', tag: 'D1', opt: 'd1', span: 4 },
      { dim: 'd6_order_status', title: '订单终态构成', tag: 'D6', kind: 'ring', span: 2 },
      { dim: 'd2_station_rank', title: '站点营收排行', tag: 'D2', kind: 'capsule', span: 2 },
      { dim: 'd4_hourly_load', title: '时段负荷分布', tag: 'D4', opt: 'd4', span: 2 },
      { dim: 'd5_kwh_distribution', title: '充电量分桶', tag: 'D5', opt: 'd5', span: 2 }
    ]
  },
  {
    key: 'device',
    name: '站点与设备',
    desc: '桩在哪、忙不忙、有没有出状况',
    panels: [
      { dim: 'd3_pile_utilization', title: '电桩利用率 Top12', tag: 'D3', opt: 'd3', span: 3 },
      { dim: 'd10_station_geo', title: '站点地理分布', tag: 'D10', opt: 'd10', span: 3 },
      { dim: 'd7_pile_status', title: '电桩在线率', tag: 'D7', opt: 'd7', span: 2 },
      { dim: 'd8_device_events', title: '设备事件时序', tag: 'D8', opt: 'd8', span: 4 }
    ]
  },
  {
    key: 'compare',
    name: '对比分析',
    // 老师明确要求「至少两个维度的数据对比分析」，单独成页最突出，
    // 答辩时可以直接翻到这一页作答
    desc: '快慢充、工作日周末、站点横向 —— 三组对比',
    panels: [
      { dim: 'c1_fast_vs_slow', title: '快充 vs 慢充', tag: 'C1 对比', opt: 'c1', span: 2 },
      { dim: 'c3_station_radar', title: '站点多指标对标', tag: 'C3 对比', opt: 'c3', span: 2 },
      { dim: 'd11_duration_distribution', title: '充电时长分布 × 快慢充', tag: 'D11', opt: 'd11', span: 2 },
      { dim: 'c2_weekday_weekend_hourly', title: '工作日 vs 周末 · 24 小时日均单量', tag: 'C2 对比', opt: 'c2', span: 6 }
    ]
  },
  {
    key: 'forecast',
    name: '负荷预测',
    desc: 'Spark MLlib 预测未来 1 / 6 / 24 小时负荷、空闲桩数与高峰时段',
    panels: [
      { dim: 'd12_load_forecast', title: 'MLlib 负荷预测 1 / 6 / 24 小时', tag: 'D12', opt: 'd12', span: 4 },
      // 空闲桩数与高峰标记放表格里——它们是整数小量，画成折线只会喧宾夺主
      { dim: 'd12_load_forecast', title: '预测明细（含空闲桩与高峰）', tag: 'D12', kind: 'table', table: 'forecast', span: 2 }
    ]
  },
  {
    key: 'carbon',
    name: '碳排放',
    desc: '排放趋势、分站横向对比与逐日明细',
    panels: [
      { dim: 'd9_carbon_daily', title: '每日碳排放与峰平谷构成', tag: 'D9', opt: 'd9', span: 4 },
      // 表格比图更适合承载「因子版本 / 完整度」这类要逐条核对的字段
      { dim: 'd13_carbon_station', title: '分站碳排放汇总', tag: 'D13', kind: 'table', table: 'carbonStation', span: 2 },
      { dim: 'd14_carbon_recent', title: '最近 15 日碳排放明细', tag: 'D14', kind: 'table', table: 'carbonRecent', span: 6 }
    ]
  },
  {
    key: 'advice',
    name: '智能运营建议',
    // 前面几页回答「过去发生了什么、未来会怎样」，这一页回答「所以该干什么」——
    // 把负荷预测翻译成分流、值守与处置动作，补上说明书里
    // 「辅助运营端做电力调配与运维值守安排」那半句。
    desc: '把预测变成动作：分流、值守、处置',
    panels: [
      { dim: 'd12_load_forecast', title: '值守 / 检修时段（1 / 6 / 24 小时）', tag: 'A2', opt: 'duty', span: 6 },
      { dim: 'd16_dispatch_advice', title: '站点分流 / 引导建议（拥堵 ≥80% 分流；前二且 ≥40% 引导）', tag: 'A1', kind: 'table', table: 'dispatchAdvice', span: 6 },
      { dim: 'd17_alert_actions', title: '预警处置清单（含模型可信度）', tag: 'A3', kind: 'table', table: 'alertActions', span: 6 }
    ]
  }
]

// 所有页面用到的维度，供一次性并发取数
export const ALL_DIMS = [...new Set(PAGES.flatMap(p => p.panels.map(x => x.dim)))]
