<template>
  <div class="status-page">
    <div class="state-code">404</div>
    <h1 class="state-title">页面不存在</h1>
    <p class="state-text">你访问的地址不存在，或已被移动。</p>
    <div class="state-actions">
      <RouterLink to="/" class="btn btn-primary">返回首页</RouterLink>
      <button class="btn btn-secondary" type="button" @click="$router.back()">返回上一页</button>
    </div>
  </div>
</template>
