<template>
  <div class="docs-page">
    <div class="page-head">
      <div>
        <h1 class="page-title">接入文档</h1>
        <p class="page-sub">外部平台（第三方应用）跳转授权接入说明 · 无需登录即可查阅</p>
      </div>
      <div class="page-actions">
        <a class="btn btn-ghost btn-sm" :href="skillUrl" download>下载 SKILL.md</a>
        <a class="btn btn-ghost btn-sm" :href="discoveryUrl" target="_blank" rel="noopener noreferrer">
          OIDC Discovery
        </a>
      </div>
    </div>

    <div v-if="loading" class="panel panel-pad muted-text">正在加载文档…</div>

    <div v-else-if="error" class="panel panel-pad">
      <div class="alert alert-danger">{{ error }}</div>
      <button class="btn btn-secondary btn-sm" type="button" @click="load">重试</button>
    </div>

    <div v-else class="docs-layout">
      <aside class="docs-toc" aria-label="目录">
        <div class="docs-toc-title">目录</div>
        <a v-for="item in toc" :key="item.slug" class="docs-toc-link"
           :class="`is-h${item.level}`" :href="`#${item.slug}`">{{ item.text }}</a>
      </aside>
      <article class="docs-body panel panel-pad" v-html="html"></article>
    </div>
  </div>
</template>

<script setup>
// The document is served by GET /api/public/docs/skill (public, read-only) and
// rendered with a small hand-rolled markdown subset — the app deliberately
// ships no markdown dependency. Input is HTML-escaped before any tags are
// emitted, so the published document cannot inject markup.
import { onMounted, ref } from 'vue'

const skillUrl = '/api/public/docs/skill?kind=external'
const discoveryUrl = '/.well-known/openid-configuration'

const html = ref('')
const toc = ref([])
const loading = ref(true)
const error = ref('')

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
}

function slugify(text) {
  return text
    .toLowerCase()
    .replace(/[`*_~]/g, '')
    .replace(/[^\p{L}\p{N}]+/gu, '-')
    .replace(/^-+|-+$/g, '') || 'section'
}

function inline(text) {
  const codes = []
  let out = escapeHtml(text)
  out = out.replace(/`([^`]+)`/g, (_, code) => {
    codes.push(code)
    return `\u0000${codes.length - 1}\u0000`
  })
  out = out.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
  out = out.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g,
    '<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>')
  out = out.replace(/\u0000(\d+)\u0000/g, (_, i) => `<code>${codes[Number(i)]}</code>`)
  return out
}

function renderTable(rows) {
  const cells = (row) => row.replace(/^\|/, '').replace(/\|$/, '').split('|').map((c) => c.trim())
  const head = cells(rows[0])
  const body = rows.slice(1)
    .filter((row) => !/^\|[\s:|-]+\|$/.test(row))
    .map(cells)
  const headHtml = head.map((c) => `<th>${inline(c)}</th>`).join('')
  const bodyHtml = body
    .map((row) => `<tr>${head.map((_, i) => `<td>${inline(row[i] || '')}</td>`).join('')}</tr>`)
    .join('')
  return `<div class="table-wrap"><table class="table"><thead><tr>${headHtml}</tr></thead><tbody>${bodyHtml}</tbody></table></div>`
}

function renderMarkdown(markdown) {
  const lines = markdown.replace(/\r\n?/g, '\n').split('\n')
  const chunks = []
  const headings = []
  const para = []
  let i = 0

  if (lines[0] && lines[0].trim() === '---') {
    i = 1
    while (i < lines.length && lines[i].trim() !== '---') i += 1
    i += 1
  }

  const flushPara = () => {
    if (para.length) {
      chunks.push(`<p>${inline(para.join(' '))}</p>`)
      para.length = 0
    }
  }

  while (i < lines.length) {
    const raw = lines[i]
    const text = raw.trim()
    if (!text) {
      flushPara()
      i += 1
      continue
    }

    if (text.startsWith('```')) {
      flushPara()
      const buf = []
      i += 1
      while (i < lines.length && !lines[i].trim().startsWith('```')) {
        buf.push(lines[i])
        i += 1
      }
      i += 1
      chunks.push(`<pre class="code-block"><code>${escapeHtml(buf.join('\n'))}</code></pre>`)
      continue
    }

    const heading = text.match(/^(#{1,6})\s+(.*)$/)
    if (heading) {
      flushPara()
      const level = heading[1].length
      const label = heading[2].trim()
      const slug = slugify(label)
      if (level === 2 || level === 3) headings.push({ level, text: label, slug })
      chunks.push(`<h${level} id="${slug}">${inline(label)}</h${level}>`)
      i += 1
      continue
    }

    if (text.startsWith('>')) {
      flushPara()
      const buf = []
      while (i < lines.length && lines[i].trim().startsWith('>')) {
        buf.push(lines[i].trim().replace(/^>\s?/, ''))
        i += 1
      }
      chunks.push(`<blockquote>${inline(buf.join(' '))}</blockquote>`)
      continue
    }

    if (text.startsWith('|')) {
      flushPara()
      const rows = []
      while (i < lines.length && lines[i].trim().startsWith('|')) {
        rows.push(lines[i].trim())
        i += 1
      }
      chunks.push(renderTable(rows))
      continue
    }

    if (/^[-*]\s+/.test(text)) {
      flushPara()
      const items = []
      while (i < lines.length && /^[-*]\s+/.test(lines[i].trim())) {
        items.push(lines[i].trim().replace(/^[-*]\s+/, ''))
        i += 1
      }
      chunks.push(`<ul>${items.map((item) => `<li>${inline(item)}</li>`).join('')}</ul>`)
      continue
    }

    if (/^\d+[.)]\s+/.test(text)) {
      flushPara()
      const items = []
      while (i < lines.length && /^\d+[.)]\s+/.test(lines[i].trim())) {
        items.push(lines[i].trim().replace(/^\d+[.)]\s+/, ''))
        i += 1
      }
      chunks.push(`<ol>${items.map((item) => `<li>${inline(item)}</li>`).join('')}</ol>`)
      continue
    }

    para.push(text)
    i += 1
  }

  flushPara()
  return { html: chunks.join('\n'), toc: headings }
}

async function load() {
  loading.value = true
  error.value = ''
  try {
    const res = await fetch(skillUrl, { credentials: 'same-origin', cache: 'no-store' })
    if (!res.ok) throw new Error(`加载失败（HTTP ${res.status}）`)
    const markdown = await res.text()
    if (!markdown.trim()) throw new Error('文档内容为空')
    const rendered = renderMarkdown(markdown)
    html.value = rendered.html
    toc.value = rendered.toc
  } catch (err) {
    error.value = err.message || '文档加载失败'
  } finally {
    loading.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.page-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.muted-text {
  color: var(--ink-soft);
  font-size: 13px;
}
.docs-layout {
  display: grid;
  grid-template-columns: 220px minmax(0, 1fr);
  gap: 32px;
  align-items: start;
}
.docs-toc {
  position: sticky;
  top: 24px;
  display: flex;
  flex-direction: column;
  gap: 6px;
  padding-right: 8px;
  border-right: 1px solid var(--line);
}
.docs-toc-title {
  font-size: 11px;
  letter-spacing: .08em;
  text-transform: uppercase;
  color: var(--ink-faint);
  margin-bottom: 4px;
}
.docs-toc-link {
  font-size: 12.5px;
  line-height: 1.5;
  color: var(--ink-soft);
  text-decoration: none;
}
.docs-toc-link:hover { color: var(--ink); }
.docs-toc-link.is-h3 { padding-left: 12px; font-size: 12px; color: var(--ink-faint); }

.docs-body { overflow-wrap: anywhere; }
.docs-body :deep(h1) {
  font-size: 22px;
  letter-spacing: -.02em;
  padding-bottom: 14px;
  margin-bottom: 18px;
  border-bottom: 1px solid var(--line);
}
.docs-body :deep(h2) {
  font-size: 17px;
  margin: 30px 0 12px;
}
.docs-body :deep(h3) {
  font-size: 14.5px;
  margin: 22px 0 8px;
}
.docs-body :deep(p),
.docs-body :deep(li) {
  font-size: 13px;
  line-height: 1.9;
  color: var(--ink-soft);
}
.docs-body :deep(p) { margin: 10px 0; }
.docs-body :deep(ul),
.docs-body :deep(ol) { margin: 8px 0; padding-left: 20px; }
.docs-body :deep(li) { margin: 4px 0; }
.docs-body :deep(strong) { color: var(--ink); }
.docs-body :deep(code) {
  padding: 2px 5px;
  border-radius: 4px;
  background: var(--fill);
  font-family: var(--mono);
  font-size: 12px;
  color: var(--ink);
}
.docs-body :deep(.code-block) {
  margin: 12px 0;
  padding: 14px 16px;
  border-radius: var(--radius-sm);
  background: #fafafa;
  border: 1px solid var(--line);
  overflow-x: auto;
}
.docs-body :deep(.code-block code) {
  padding: 0;
  background: none;
  font-size: 12px;
  line-height: 1.7;
  white-space: pre;
}
.docs-body :deep(blockquote) {
  margin: 12px 0;
  padding: 2px 0 2px 14px;
  border-left: 3px solid var(--line-strong);
  color: var(--ink-soft);
  font-size: 13px;
  line-height: 1.85;
}
.docs-body :deep(a) { color: var(--brand-ink); }
.docs-body :deep(th) { white-space: nowrap; }

@media (max-width: 720px) {
  .docs-layout { grid-template-columns: minmax(0, 1fr); gap: 20px; }
  .docs-toc {
    position: static;
    border-right: none;
    border-bottom: 1px solid var(--line);
    padding: 0 0 14px;
  }
  .docs-body { padding: 18px; }
}
</style>
