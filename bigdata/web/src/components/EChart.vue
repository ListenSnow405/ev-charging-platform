<script setup>
// ECharts 通用容器。
// 三件必须做对的事：容器尺寸变了要 resize、组件销毁要 dispose、option 变了要重绘。
// 少任何一件，大屏跑久了就会内存泄漏或图表糊成一团。
import * as echarts from 'echarts'
import { onMounted, onBeforeUnmount, ref, watch } from 'vue'

const props = defineProps({ option: { type: Object, required: true } })
const el = ref(null)
let chart = null
let ro = null

function render () {
  if (!chart || !props.option) return
  // notMerge=true：维度切换时若沿用旧 option 合并，残留的 series 会画出幽灵数据
  chart.setOption(props.option, true)
}

onMounted(() => {
  chart = echarts.init(el.value)
  render()
  // 用 ResizeObserver 而不是 window.resize：大屏是网格布局，
  // 面板自身尺寸变化（如浏览器缩放、父容器伸缩）不一定触发 window.resize
  ro = new ResizeObserver(() => chart && chart.resize())
  ro.observe(el.value)
})

onBeforeUnmount(() => {
  if (ro) { ro.disconnect(); ro = null }
  if (chart) { chart.dispose(); chart = null }
})

watch(() => props.option, render, { deep: true })
</script>

<template><div ref="el" class="echart"></div></template>

<style scoped>
.echart { width: 100%; height: 100%; }
</style>
