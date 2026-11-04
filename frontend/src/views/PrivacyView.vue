<template>
  <div>
    <div class="page-head">
      <div>
        <h1 class="page-title">隐私说明</h1>
        <p class="page-sub">统一身份认证只收集登录所必需的最少信息。</p>
      </div>
    </div>
    <div class="panel privacy-panel">
      <p class="privacy-line">
        我们保存你的邮箱、显示名、头像与登录会话，仅用于账号登录与平台间身份识别；
        不追踪浏览行为，不向接入应用提供你未授权的信息。
      </p>
    </div>
  </div>
</template>

<style scoped>
.privacy-panel {
  padding: 24px;
}
.privacy-line {
  margin: 0;
  font-size: 13px;
  line-height: 1.9;
  color: var(--ink-soft);
}
@media (max-width: 720px) {
  .privacy-panel { padding: 18px; }
}
</style>
