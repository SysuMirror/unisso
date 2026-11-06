<template>
  <section class="skill-prompt">
    <div class="skill-prompt-main">
      <div class="skill-prompt-head">
        <span class="skill-prompt-badge">一键接入</span>
        <h2 class="skill-prompt-title">把这句话交给 AI，自动完成接入与开发</h2>
      </div>
      <p class="skill-prompt-sub">
        复制下面的提示词，粘贴给 Claude / Cursor 等编码助手；它会读取官方接入规范，
        在你的项目里实现 UniSSO 登录并自测全链路。
      </p>
      <pre class="skill-prompt-code"><code>{{ prompt }}</code></pre>
    </div>
    <div class="skill-prompt-side">
      <button class="btn btn-primary" type="button" @click="copy">
        {{ copied ? '已复制 ✓' : '复制提示词' }}
      </button>
      <a class="btn btn-secondary btn-sm" :href="skillUrl" download>下载 SKILL.md</a>
      <a class="skill-prompt-link" :href="skillUrl" target="_blank" rel="noopener noreferrer">
        查看接入规范 ↗
      </a>
    </div>
  </section>
</template>

<script setup>
// One-liner prompt that hands the whole integration job to a coding agent.
// The prompt points at the public, hot-read SKILL document so it never drifts
// from the served contract.
import { ref } from 'vue'

const skillUrl = '/api/public/docs/skill?kind=external'
const copied = ref(false)
let copyTimer = null

const prompt = `请阅读并严格遵循 ${typeof window !== 'undefined' ? window.location.origin : ''}${skillUrl} 的 UniSSO 接入规范，帮我把当前项目接入 UniSSO 单点登录（OAuth2 授权码 + PKCE S256）：
1. 先通读该规范（端点、PKCE、错误契约、验收清单）；
2. 在 https://sso.ssemarket.cn/developer 点「注册接入」自助创建对外应用（学院成员提交即生效），登记回调地址，拿到 client_id / client_secret（secret 只显示一次）；
3. 按规范实现：跳转 /authorize → 回调校验 state → 后端用 code 换 token → 调 /api/oauth/userinfo 建立本地会话；client_secret 只留在后端，回调地址逐字符与登记值一致；
4. 自测「授权 → 回调 → 换 token → 取用户信息」全链路通过后交付。`

async function copy() {
  const text = prompt
  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text)
    } else {
      const area = document.createElement('textarea')
      area.value = text
      area.style.position = 'fixed'
      area.style.opacity = '0'
      document.body.appendChild(area)
      area.select()
      document.execCommand('copy')
      document.body.removeChild(area)
    }
  } catch {
    // Clipboard can be blocked (insecure context / permission); fall back to
    // selecting the text so the user can copy manually.
    const node = document.querySelector('.skill-prompt-code code')
    if (node && window.getSelection) {
      const range = document.createRange()
      range.selectNodeContents(node)
      const sel = window.getSelection()
      sel.removeAllRanges()
      sel.addRange(range)
    }
    return
  }
  copied.value = true
  clearTimeout(copyTimer)
  copyTimer = setTimeout(() => { copied.value = false }, 2000)
}
</script>

<style scoped>
.skill-prompt {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 20px;
  margin: 0 0 28px;
  padding: 18px 20px;
  border: 1px solid var(--brand-soft, #eaf1ff);
  border-left: 3px solid var(--brand, #3370ff);
  border-radius: var(--radius, 12px);
  background: linear-gradient(180deg, var(--brand-tint, #f5f8ff), #fff);
}
.skill-prompt-main { min-width: 0; flex: 1; }
.skill-prompt-head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.skill-prompt-badge {
  display: inline-block;
  padding: 2px 9px;
  border-radius: 999px;
  background: var(--brand, #3370ff);
  color: #fff;
  font-size: 11px;
  font-weight: 600;
  letter-spacing: .04em;
}
.skill-prompt-title {
  font-size: 15px;
  font-weight: 650;
  letter-spacing: -.01em;
  color: var(--ink, #171717);
}
.skill-prompt-sub {
  margin-top: 7px;
  font-size: 12.5px;
  line-height: 1.75;
  color: var(--ink-soft, #5f6368);
}
.skill-prompt-code {
  margin: 12px 0 0;
  padding: 12px 14px;
  border-radius: var(--radius-sm, 8px);
  background: #fbfcff;
  border: 1px solid var(--line, #e8e8e8);
  overflow-x: auto;
}
.skill-prompt-code code {
  font-family: var(--mono, monospace);
  font-size: 12px;
  line-height: 1.7;
  color: var(--ink, #171717);
  white-space: pre-wrap;
  word-break: break-word;
}
.skill-prompt-side {
  display: flex;
  flex-direction: column;
  align-items: stretch;
  gap: 8px;
  flex: 0 0 auto;
  min-width: 132px;
}
.skill-prompt-link { font-size: 12px; text-align: center; color: var(--brand-ink, #245bdb); }

@media (max-width: 720px) {
  .skill-prompt { flex-direction: column; }
  .skill-prompt-side { width: 100%; min-width: 0; }
}
</style>
