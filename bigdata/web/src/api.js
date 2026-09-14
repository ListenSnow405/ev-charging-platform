// Flask 只读 API 的访问层。
// 开发期由 vite.config.js 的 proxy 转到 127.0.0.1:5000，所以路径统一写 /api。

const BASE = '/api'

async function get (path) {
  const r = await fetch(BASE + path, { cache: 'no-store' })
  const body = await r.json().catch(() => ({ code: -1, msg: '响应不是合法 JSON' }))
  // 服务端用 {code,msg,data} 统一包装，code!==0 一律当失败，
  // 不要因为 HTTP 200 就以为成功——业务错误也是 200 之外的状态码加 code
  if (!r.ok || body.code !== 0) {
    throw new Error(`${path} 失败：code=${body.code} ${body.msg || r.status}`)
  }
  return body
}

export const fetchOverview = () => get('/overview').then(b => b.data)
export const fetchDimensions = () => get('/dimensions').then(b => b.data)
export const fetchDimension = name => get(`/dimension/${name}`).then(b => b.data)

// 金额一律整数「分」、电量为「度×100」（CLAUDE.md 5.2 第 7 条）。
// 除以 100 只在这一层做，组件里不再各自换算，避免口径分叉。
export const fenToYuan = fen => (Number(fen) || 0) / 100
export const kwhOf = x100 => (Number(x100) || 0) / 100
export const wan = n => (Number(n) || 0) / 10000
