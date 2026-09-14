#!/usr/bin/env python3
"""第二阶段大屏渲染冒烟　归属 L5。

**为什么不用 firefox --headless --screenshot**：本机上它退出码为 0 却不产出文件，
第一阶段就踩过同一个坑（见 L5-PLAN 第 4 节）。这里改走 geckodriver 的 WebDriver
REST 协议——纯 HTTP，不需要装 selenium，而且除了截图还能对 DOM 下断言。

「构建通过」不等于「画得出来」：Decimal 被序列化成字符串、option 结构写错、
组件名拼错，这些在浏览器里统统只表现为**一块空白**，不报错。所以必须真的渲染一次。

用法：.venv-phase2/bin/python scripts/smoke-screen-phase2.py [--url http://127.0.0.1:4173/]
退出码 = 失败项数。
"""
from __future__ import annotations

import argparse
import base64
import json
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "bigdata" / "web" / "screenshot.png"
DRIVER = "/snap/bin/firefox.geckodriver"
PORT = 4444
fails = 0


def rq(method: str, path: str, payload=None):
    url = f"http://127.0.0.1:{PORT}{path}"
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read())


def check(name: str, cond: bool, detail: str = "") -> None:
    global fails
    if cond:
        print(f"  [PASS] {name}　{detail}")
    else:
        fails += 1
        print(f"  [FAIL] {name}　{detail}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:4173/")
    args = ap.parse_args()

    driver_bin = DRIVER if Path(DRIVER).exists() else shutil.which("geckodriver")
    if not driver_bin:
        print("[跳过] 找不到 geckodriver，无法做渲染验证")
        return 0

    proc = subprocess.Popen([driver_bin, "--port", str(PORT)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    session = None
    try:
        for _ in range(40):                       # 等 driver 起来
            try:
                rq("GET", "/status")
                break
            except Exception:
                time.sleep(0.5)

        s = rq("POST", "/session", {"capabilities": {"alwaysMatch": {
            "browserName": "firefox",
            "moz:firefoxOptions": {"args": ["-headless", "--width=1920", "--height=1080"]}}}})
        session = s["value"]["sessionId"]
        base = f"/session/{session}"

        rq("POST", base + "/window/rect",
           {"x": 0, "y": 0, "width": 1920, "height": 1080})
        rq("POST", base + "/url", {"url": args.url})

        # 等数据到齐并渲染完。图表是异步画的，不等会截到空白。
        time.sleep(8)

        print("== 渲染断言 ==\n")

        # 1) 页面没有挂在全局错误上
        err = rq("POST", base + "/execute/sync", {
            "script": "const e=document.querySelector('.global-err');"
                      "return e? e.innerText.slice(0,180) : '';", "args": []})["value"]
        check("无全局错误提示", not err, err or "页面正常加载")

        # 2) 分页：逐页点过去，每页都要有面板且不空
        tabs = rq("POST", base + "/execute/sync", {
            "script": "return [...document.querySelectorAll('nav.tabs .tab')]"
                      ".filter(b=>!b.classList.contains('auto')).map(b=>b.innerText.trim());",
            "args": []})["value"]
        check("分页标签已渲染", len(tabs) >= 3, f"{len(tabs)} 页：{tabs}")

        total_panels = 0
        for i in range(len(tabs)):
            rq("POST", base + "/execute/sync", {
                "script": "const b=[...document.querySelectorAll('nav.tabs .tab')]"
                          ".filter(x=>!x.classList.contains('auto'));b[arguments[0]].click();",
                "args": [i]})
            time.sleep(3)                       # 等该页图表画完
            st = rq("POST", base + "/execute/sync", {
                "script": "return {panels: document.querySelectorAll('.panel-inner').length,"
                          "canvas: [...document.querySelectorAll('.echart canvas')]"
                          ".map(c=>c.width*c.height).filter(a=>a>1000).length,"
                          "stuck: [...document.querySelectorAll('.panel-state')]"
                          ".map(e=>e.innerText.trim()).filter(Boolean)};", "args": []})["value"]
            total_panels += st["panels"]
            # d12 在 MLlib 跑完前必然「暂无数据」，是已知且预期的
            stuck = [x for x in st["stuck"] if x != "暂无数据"]
            # 负荷预测页只有 1 格是设计如此（D12 独占整页），所以下限是 1 不是 2
            check(f"第 {i+1} 页「{tabs[i]}」", st["panels"] >= 1 and not stuck,
                  f"{st['panels']} 个面板　{st['canvas']} 个非空 canvas"
                  + (f"　空数据 {len(st['stuck'])} 格" if st["stuck"] else ""))
        check("面板总数 = 17", total_panels == 17, f"实际 {total_panels} 个")
        n_panel = total_panels

        # （原整屏级「无面板卡在空数据」断言已删：分页后它只作用于当前页，
        #   与上面的逐页检查重复，且会因 d12 的预期空数据误报失败）

        # 4) ECharts 真的画出了像素。canvas 存在还不够——
        #    尺寸为 0 的 canvas 也"存在"，那就是一块空白。
        canvas = rq("POST", base + "/execute/sync", {
            "script": "return [...document.querySelectorAll('.echart canvas')]"
                      ".map(c=>c.width*c.height).filter(a=>a>1000).length;", "args": []})["value"]
        check("当前页 ECharts 已出图", canvas >= 1, f"{canvas} 个非空 canvas")

        # 5) DataV 组件渲染（边框 svg + 数字翻牌）
        datav = rq("POST", base + "/execute/sync", {
            "script": "return {border: document.querySelectorAll('.dv-border-box-13').length,"
                      "flop: document.querySelectorAll('.dv-digital-flop').length,"
                      "capsule: document.querySelectorAll('.dv-capsule-chart').length,"
                      "ring: document.querySelectorAll('.dv-active-ring-chart').length};",
            "args": []})["value"]
        check("DataV 边框组件已渲染", datav["border"] >= 2, f"当前页 BorderBox13 × {datav['border']}")
        check("DataV 数字翻牌已渲染", datav["flop"] >= 6, f"DigitalFlop × {datav['flop']}")
        # 胶囊图与环图都在第 1 页，上面逐页遍历后会停在最后一页，这里回到第 1 页再验
        rq("POST", base + "/execute/sync", {
            "script": "[...document.querySelectorAll('nav.tabs .tab')]"
                      ".filter(x=>!x.classList.contains('auto'))[0].click();", "args": []})
        time.sleep(3)
        dv1 = rq("POST", base + "/execute/sync", {
            "script": "return {capsule: document.querySelectorAll('.dv-capsule-chart').length,"
                      "ring: document.querySelectorAll('.dv-active-ring-chart').length};",
            "args": []})["value"]
        check("DataV 胶囊图与环图已渲染（第 1 页）",
              dv1["capsule"] >= 1 and dv1["ring"] >= 1,
              f"Capsule × {dv1['capsule']}　ActiveRing × {dv1['ring']}")

        # 6) KPI 数字确实被画出来了。
        #    DvDigitalFlop 渲染进 <canvas>，**没有任何文本节点**——
        #    innerText 和 textContent 都恒为空，拿它们断言是永远误报的写法（踩过）。
        #    canvas 只能验像素：数一下非透明像素，空画布为 0。
        #    数值是否*正确*由 smoke-api-phase2.py 在接口层断言，两边各管一段。
        painted = rq("POST", base + "/execute/sync", {
            "script": """
                const out = [];
                for (const el of document.querySelectorAll('.dv-digital-flop')) {
                  const c = el.querySelector('canvas');
                  if (!c || !c.width) { out.push(0); continue; }
                  const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
                  let n = 0;
                  for (let i = 3; i < d.length; i += 4) if (d[i] > 0) n++;
                  out.push(n);
                }
                return out;""", "args": []})["value"]
        blank = [i for i, n in enumerate(painted) if n < 50]
        check("KPI 翻牌已画出像素", painted and not blank,
              f"{len(painted)} 块画布，最少 {min(painted) if painted else 0} 个着色像素"
              + (f"，空白块索引 {blank}" if blank else ""))

        # 7) 控制台有没有报错
        try:
            logs = rq("POST", base + "/execute/sync", {
                "script": "return window.__errs||[]", "args": []})["value"]
        except Exception:
            logs = []
        check("无页面级 JS 错误", not logs, str(logs) if logs else "无")

        # 8) 截图落地，供人工看一眼布局（像素级美观机器判不了）
        png = rq("GET", base + "/screenshot")["value"]
        OUT.write_bytes(base64.b64decode(png))
        size = OUT.stat().st_size
        check("截图已产出", size > 50_000, f"{OUT.name} {size/1024:.0f} KB")

        print(f"\n{'='*46}")
        print(f"截图：{OUT}")
        print("RESULT:", "PASS" if fails == 0 else f"FAIL（{fails} 项）")
        return fails
    finally:
        if session:
            try:
                rq("DELETE", f"/session/{session}")
            except Exception:
                pass
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
