import { Navigate } from 'react-router-dom'
import { isAuthenticated } from '../services/api.js'

// Halaman yang butuh login: tanpa token / token kedaluwarsa => ke /login
export function RequireAuth({ children }) {
  return isAuthenticated() ? children : <Navigate to="/login" replace />
}

// Halaman login: kalau sudah login, langsung ke dashboard
export function GuestOnly({ children }) {
  return isAuthenticated() ? <Navigate to="/dashboard" replace /> : children
}
