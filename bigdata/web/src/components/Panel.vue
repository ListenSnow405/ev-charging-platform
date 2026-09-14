<script setup>
// 图表面板：DataV 边框 + 标题 + 内容槽。
// 统一在这里处理"加载中/出错/无数据"三态——每个图各写一遍必然有遗漏，
// 而大屏上一个空白面板和一个加载中的面板，肉眼分不出区别。
defineProps({
  title: { type: String, required: true },
  tag: { type: String, default: '' },      // 维度编号，答辩时便于对照
  loading: Boolean,
  error: { type: String, default: '' },
  empty: Boolean
})
</script>

<template>
  <DvBorderBox13 class="panel">
    <div class="panel-inner">
      <div class="panel-head">
        <span class="panel-title">{{ title }}</span>
        <span v-if="tag" class="panel-tag">{{ tag }}</span>
      </div>
      <div class="panel-body">
        <div v-if="loading" class="panel-state"><DvLoading>加载中</DvLoading></div>
        <div v-else-if="error" class="panel-state err">{{ error }}</div>
        <div v-else-if="empty" class="panel-state">暂无数据</div>
        <slot v-else />
      </div>
    </div>
  </DvBorderBox13>
</template>

<style scoped>
.panel { width: 100%; height: 100%; }
.panel-inner { display: flex; flex-direction: column; height: 100%; padding: 10px 14px 8px; box-sizing: border-box; }
.panel-head { display: flex; align-items: center; gap: 8px; flex: 0 0 auto; margin-bottom: 2px; }
.panel-title { font-size: 14px; font-weight: 600; color: #e6f7ff; letter-spacing: .5px; }
.panel-tag { font-size: 10px; color: #36cfc9; border: 1px solid rgba(54,207,201,.45); border-radius: 3px; padding: 0 4px; }
.panel-body { flex: 1 1 auto; min-height: 0; }
.panel-state { display: flex; align-items: center; justify-content: center; height: 100%; color: #7f9cb8; font-size: 12px; }
.panel-state.err { color: #ff7875; text-align: center; padding: 0 12px; line-height: 1.5; }
</style>
