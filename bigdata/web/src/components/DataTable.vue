<script setup>
// 大屏表格：DataV 的 DvScrollBoard 封装。
//
// 行数超过 rowNum 时它会自动轮播滚动，这正是大屏要的——
// 用普通 <table> 加滚动条在投影上没人能操作。
//
// 数值一律在传入前格式化好（本组件不做单位换算），
// 保持「换算只在 api.js / charts.js 做一次」的口径。
import { computed } from 'vue'

const props = defineProps({
  header: { type: Array, required: true },
  rows: { type: Array, required: true },      // 二维数组，每行元素个数需与 header 一致
  columnWidth: { type: Array, default: () => [] },
  align: { type: Array, default: () => [] },
  rowNum: { type: Number, default: 6 }
})

const config = computed(() => ({
  header: props.header,
  data: props.rows,
  index: true,
  indexHeader: '#',
  columnWidth: props.columnWidth,
  align: props.align,
  rowNum: props.rowNum,
  headerHeight: 32,
  headerBGC: 'rgba(54, 207, 201, .14)',
  oddRowBGC: 'rgba(20, 45, 80, .35)',
  evenRowBGC: 'transparent',
  waitTime: 2500
}))
</script>

<template>
  <DvScrollBoard :config="config" class="tbl" />
</template>

<style scoped>
.tbl { width: 100%; height: 100%; font-size: 13px; }
/* DvScrollBoard 的表头默认字色偏暗，在深色底上不够清楚 */
.tbl :deep(.header-item) { color: #36cfc9; font-weight: 600; letter-spacing: .5px; }
.tbl :deep(.row-item) { color: #c9d6e5; }
.tbl :deep(.ceil) { padding: 0 6px; }
</style>
