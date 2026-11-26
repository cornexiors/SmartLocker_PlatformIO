import { useState } from 'react'
import { Link, useLocation, useNavigate } from 'react-router-dom'
import { login } from '../services/api.js'

// Pesan untuk user: tidak menampilkan detail internal backend.
function friendlyError(err) {
  if (err.status === 0) return 'Tidak dapat terhubung ke layanan. Periksa koneksi Anda lalu coba lagi.'
  if (err.status >= 500) return 'Terjadi gangguan pada layanan. Coba lagi beberapa saat lagi.'
  return err.message
}

function BrandMark() {
  return (
    <svg className="brand-mark" viewBox="0 0 40 40" width="48" height="48" aria-hidden="true">
      <rect width="40" height="40" rx="10" fill="#1d1d1f" />
      <g fill="none" stroke="#fff" strokeWidth="2.2" strokeLinecap="round" strokeLinejoin="round">
        <rect x="11" y="18" width="18" height="12" rx="3" />
        <path d="M15 18v-3a5 5 0 0 1 10 0v3" />
      </g>
    </svg>
  )
}

export default function LoginPage() {
  const navigate = useNavigate()
  const location = useLocation()
  const registered = location.state?.registered
  const [username, setUsername] = useState(registered ?? '')
  const [password, setPassword] = useState('')
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState('')

  async function handleSubmit(e) {
    e.preventDefault()
    if (loading) return
    setError('')
    setLoading(true)
    try {
      await login(username.trim(), password)
      navigate('/dashboard', { replace: true })
    } catch (err) {
      setError(friendlyError(err))
      setLoading(false)
    }
  }

  return (
    <main className="auth">
      <form className="auth__card" onSubmit={handleSubmit}>
        <div className="auth__brand">
          <BrandMark />
          <h1>Smart Locker</h1>
          <p>Masuk untuk mengelola locker Anda.</p>
        </div>

        {registered && !error && (
          <div className="alert alert--ok" role="status">Akun berhasil dibuat. Silakan login.</div>
        )}
        {error && <div className="alert alert--error" role="alert">{error}</div>}

        <div className="field">
          <label htmlFor="username">Username</label>
          <input
            id="username"
            autoComplete="username"
            value={username}
            onChange={(e) => setUsername(e.target.value)}
            disabled={loading}
            required
            autoFocus={!registered}
          />
        </div>

        <div className="field">
          <label htmlFor="password">Password</label>
          <input
            id="password"
            type="password"
            autoComplete="current-password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            disabled={loading}
            required
            autoFocus={Boolean(registered)}
          />
        </div>

        <button className="btn btn--primary btn--block btn--lg" type="submit" disabled={loading || !username || !password}>
          {loading ? <><span className="spinner" aria-hidden="true" />Memproses…</> : 'Login'}
        </button>

        <p className="auth__alt">Belum punya akun? <Link to="/register">Daftar</Link></p>
      </form>
    </main>
  )
}
