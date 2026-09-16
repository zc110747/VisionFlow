import { createRouter, createWebHistory } from 'vue-router'
import HomeView from '@/views/HomeView.vue'
import CameraView from '@/views/CameraView.vue'

const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', name: 'home', component: HomeView },
    { path: '/camera/:id', name: 'camera', component: CameraView, props: true }
  ]
})

export default router
