<template>
  <div>
    <div class="page-head">
      <div>
        <h1 class="page-title">隐私政策</h1>
        <p class="page-sub">人脸二次验证与个人信息处理说明。</p>
      </div>
    </div>
    <div class="panel privacy-panel">
      <FaceAgreement />
    </div>
  </div>
</template>

<script setup>
import FaceAgreement from '../components/FaceAgreement.vue'
</script>

<style scoped>
.privacy-panel {
  padding: 24px;
}
@media (max-width: 720px) {
  .privacy-panel { padding: 18px; }
}
</style>
