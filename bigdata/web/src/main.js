import { createApp } from 'vue'
import DataVVue3 from '@kjgl77/datav-vue3'
// DataV 的样式必须手动引入：它的 package.json 里没有 style 字段，
// 但 dist/style.css 确实存在。不引的话边框与装饰组件全是裸的。
import '@kjgl77/datav-vue3/dist/style.css'
import App from './App.vue'
import './style.css'

createApp(App).use(DataVVue3).mount('#app')
