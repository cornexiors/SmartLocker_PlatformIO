import { useEffect, useState } from 'react'
import { Link, useNavigate } from 'react-router-dom'
import { requestRegistrationOtp, verifyRegistrationOtp } from '../services/api.js'

const USERNAME_RE = /^[A-Za-z0-9_]{3,32}$/

function BrandMark() {
  return <svg className="brand-mark" viewBox="0 0 40 40" width="48" height="48" aria-hidden="true">
    <rect width="40" height="40" rx="10" fill="#1d1d1f" />
    <g fill="none" stroke="#fff" strokeWidth="2.2" strokeLinecap="round" strokeLinejoin="round">
      <rect x="11" y="18" width="18" height="12" rx="3" /><path d="M15 18v-3a5 5 0 0 1 10 0v3" />
    </g>
  </svg>
}

function validate({ username, email, password, confirm }) {
  const errors = {}
  if (!USERNAME_RE.test(username)) errors.username = 'Username 3–32 karakter, hanya huruf, angka, dan underscore.'
  if (!email || !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) errors.email = 'Masukkan email yang valid.'
  if (password.length < 8 || password.length > 128) errors.password = 'Password harus 8–128 karakter.'
  if (confirm !== password) errors.confirm = 'Konfirmasi password tidak sama.'
  return errors
}

export default function RegisterPage() {
  const navigate = useNavigate()
  const [form, setForm] = useState({ username: '', email: '', password: '', confirm: '', otp: '' })
  const [errors, setErrors] = useState({})
  const [submitted, setSubmitted] = useState(false)
  const [step, setStep] = useState('details')
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState('')
  const [resendIn, setResendIn] = useState(0)

  useEffect(() => {
    if (!resendIn) return undefined
    const timer = setInterval(() => setResendIn((value) => Math.max(0, value - 1)), 1000)
    return () => clearInterval(timer)
  }, [resendIn])

  function update(field, value) {
    const next = { ...form, [field]: value }
    setForm(next)
    if (submitted && step === 'details') setErrors(validate(next))
  }

  async function resend() {
    if (loading || resendIn) return
    setLoading(true)
    setError('')
    try {
      await requestRegistrationOtp(form.username, form.email, form.password)
      setResendIn(60)
    } catch (err) {
      setError(err.status >= 500 ? 'Layanan sedang bermasalah. Coba lagi nanti.' : err.message)
    } finally {
      setLoading(false)
    }
  }

  async function handleSubmit(e) {
    e.preventDefault()
    if (loading) return
    setError('')
    setSubmitted(true)
    const found = validate(form)
    setErrors(found)
    if (Object.keys(found).length) return
    setLoading(true)
    try {
      await requestRegistrationOtp(form.username, form.email, form.password)
      setStep('otp')
      setResendIn(60)
      setLoading(false)
    } catch (err) {
      setError(err.status >= 500 ? 'Layanan sedang bermasalah. Coba lagi nanti.' : err.message)
      setLoading(false)
    }
  }

  async function handleVerify(e) {
    e.preventDefault()
    if (loading || !/^\d{6}$/.test(form.otp)) {
      if (!/^\d{6}$/.test(form.otp)) setError('Masukkan kode OTP 6 digit.')
      return
    }
    setLoading(true)
    setError('')
    try {
      await verifyRegistrationOtp(form.username, form.email, form.otp)
      navigate('/login', { replace: true, state: { registered: form.username } })
    } catch (err) {
      setError(err.status >= 500 ? 'Layanan sedang bermasalah. Coba lagi nanti.' : err.message)
      setLoading(false)
    }
  }

  return <main className="auth">
    <form className="auth__card" onSubmit={step === 'details' ? handleSubmit : handleVerify} noValidate>
      <div className="auth__brand"><BrandMark /><h1>Buat akun</h1>
        <p>{step === 'details' ? 'Daftar untuk mulai memakai locker.' : `Kode verifikasi dikirim ke ${form.email}.`}</p>
      </div>
      {error && <div className="alert alert--error" role="alert">{error}</div>}
      {step === 'details' ? <>
        <div className="field"><label htmlFor="username">Username</label>
          <input id="username" autoComplete="username" value={form.username} onChange={(e) => update('username', e.target.value)} disabled={loading} autoFocus />
          <p className={errors.username ? 'field__error' : 'field__hint'}>{errors.username ?? '3–32 karakter: huruf, angka, atau underscore.'}</p>
        </div>
        <div className="field"><label htmlFor="email">Email</label>
          <input id="email" type="email" autoComplete="email" value={form.email} onChange={(e) => update('email', e.target.value)} disabled={loading} />
          {errors.email && <p className="field__error">{errors.email}</p>}
        </div>
        <div className="field"><label htmlFor="password">Password</label>
          <input id="password" type="password" autoComplete="new-password" value={form.password} onChange={(e) => update('password', e.target.value)} disabled={loading} />
          <p className={errors.password ? 'field__error' : 'field__hint'}>{errors.password ?? 'Minimal 8 karakter.'}</p>
        </div>
        <div className="field"><label htmlFor="confirm">Konfirmasi password</label>
          <input id="confirm" type="password" autoComplete="new-password" value={form.confirm} onChange={(e) => update('confirm', e.target.value)} disabled={loading} />
          {errors.confirm && <p className="field__error">{errors.confirm}</p>}
        </div>
      </> : <div className="field"><label htmlFor="otp">Kode OTP</label>
        <input id="otp" inputMode="numeric" maxLength="6" autoComplete="one-time-code" value={form.otp} onChange={(e) => update('otp', e.target.value.replace(/\D/g, '').slice(0, 6))} disabled={loading} autoFocus />
        <p className="field__hint">Kode berlaku selama 5 menit. Minta kode baru setelah cooldown berakhir.</p>
      </div>}
      <button className="btn btn--primary btn--block btn--lg" type="submit" disabled={loading}>
        {loading ? <><span className="spinner" aria-hidden="true" />Memproses…</> : (step === 'details' ? 'Kirim kode verifikasi' : 'Verifikasi dan buat akun')}
      </button>
      {step === 'otp' && <button className="btn btn--block" type="button" onClick={resend} disabled={loading || resendIn > 0}>
        {resendIn ? `Kirim ulang dalam ${resendIn} detik` : 'Kirim ulang kode'}
      </button>}
      <p className="auth__alt">Sudah punya akun? <Link to="/login">Kembali ke login</Link></p>
    </form>
  </main>
}
