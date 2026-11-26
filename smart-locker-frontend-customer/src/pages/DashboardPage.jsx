import { useCallback, useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import {
  claimLocker, clearSession, getCommand, getLockers, getMe, getProfile,
  getTokenExpiry, releaseLocker, extendLocker, finishLocker, sendLockerCommand,
  getForgottenLockers, resolveForgottenLocker, setDeviceCredentials,
} from '../services/api.js'
import LockerCard from '../components/LockerCard.jsx'
import StatusItem from '../components/StatusItem.jsx'

const REFRESH_MS = 5000        // refresh status locker berkala
const POLL_MS = 1500           // polling status command
const POLL_TIMEOUT_MS = 30000  // batas aman polling
const FINAL_STATUSES = ['ACKNOWLEDGED', 'FAILED', 'TIMED_OUT', 'MANUAL_READY']

const COMMAND_STATUS = {
  PENDING: { text: 'Menunggu konfirmasi perangkat…', tone: 'pending' },
  ACKNOWLEDGED: { text: 'Perangkat sudah mengonfirmasi perintah.', tone: 'ok' },
  FAILED: { text: 'Perangkat menolak perintah.', tone: 'bad' },
  TIMED_OUT: { text: 'Waktu habis, perangkat tidak merespons.', tone: 'bad' },
  MANUAL_READY: { text: 'Perangkat tidak merespons, locker siap dibuka manual.', tone: 'warn' },
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

// Pesan untuk user biasa: tanpa detail internal backend.
function friendlyError(err) {
  if (err.status === 0) return 'Tidak dapat terhubung ke layanan. Periksa koneksi Anda lalu coba lagi.'
  if (err.status >= 500) return 'Terjadi gangguan pada layanan. Coba lagi beberapa saat lagi.'
  return err.message
}

export default function DashboardPage() {
  const navigate = useNavigate()
  const alive = useRef(true)
  const [profile, setProfile] = useState(getProfile)
  const [overview, setOverview] = useState(null)
  const [loadError, setLoadError] = useState(null) // { detail, friendly }
  const [updatedAt, setUpdatedAt] = useState(null)
  const [ops, setOps] = useState({}) // { [lockerId]: { busy, result } }
  const [forgotten, setForgotten] = useState([])
  const [devicePin, setDevicePin] = useState('')
  const [deviceRfid, setDeviceRfid] = useState('')
  const [credentialBusy, setCredentialBusy] = useState(false)
  const [credentialResult, setCredentialResult] = useState(null)

  const logout = useCallback(() => {
    clearSession()
    navigate('/login', { replace: true })
  }, [navigate])

  const refresh = useCallback(async () => {
    try {
      const data = await getLockers()
      if (!alive.current) return
      setOverview(data)
      setLoadError(null)
      setUpdatedAt(new Date())
    } catch (err) {
      if (!alive.current) return
      if (err.status === 401) return logout()
      setLoadError({ detail: err.message, friendly: friendlyError(err) })
    }
  }, [logout])

  const refreshForgotten = useCallback(async () => {
    if (profile?.role !== 'admin') return
    try {
      const data = await getForgottenLockers()
      if (alive.current) setForgotten(data.lockers ?? [])
    } catch (err) {
      if (err.status === 401) logout()
    }
  }, [profile?.role, logout])

  // Profil (GET /me) + refresh locker awal dan berkala
  useEffect(() => {
    alive.current = true
    getMe()
      .then((me) => alive.current && setProfile(me))
      .catch((err) => {
        if (!alive.current) return
        if (err.status === 401) return logout()
        setLoadError({ detail: err.message, friendly: friendlyError(err) })
      })
    refresh()
    refreshForgotten()
    const id = setInterval(refresh, REFRESH_MS)
    const forgottenId = setInterval(refreshForgotten, REFRESH_MS)
    return () => {
      alive.current = false
      clearInterval(id)
      clearInterval(forgottenId)
    }
  }, [refresh, refreshForgotten, logout])

  // Logout otomatis saat token kedaluwarsa
  useEffect(() => {
    const exp = getTokenExpiry()
    if (!exp) return
    const id = setTimeout(logout, Math.max(exp - Date.now(), 0))
    return () => clearTimeout(id)
  }, [logout])

  const patch = (lockerId, values) =>
    setOps((o) => ({ ...o, [lockerId]: { ...o[lockerId], ...values } }))

  /* ---------- Claim ---------- */
  async function handleClaim(locker) {
    const id = locker.locker_id
    if (ops[id]?.busy) return
    if (!profile?.device_credentials_configured) {
      patch(id, { busy: null, result: { title: 'Atur PIN perangkat', text: 'Simpan PIN pada bagian PIN & RFID perangkat sebelum memakai locker.', tone: 'warn' } })
      return
    }
    patch(id, { busy: 'CLAIM', result: null })
    try {
      await claimLocker(id)
      if (!alive.current) return
      patch(id, { busy: null, result: { title: 'Pakai locker', text: 'Locker berhasil dipakai. Sekarang Anda bisa mengontrolnya.', tone: 'ok' } })
      await refresh()
    } catch (err) {
      if (!alive.current) return
      if (err.status === 401) return logout()
      const text = err.status === 409 ? 'Locker baru saja dipakai user lain.' : friendlyError(err)
      patch(id, { busy: null, result: { title: 'Pakai locker gagal', text, tone: 'bad' } })
      if (err.status === 409) refresh()
    }
  }

  async function handleCredentialsSubmit(event) {
    event.preventDefault()
    if (credentialBusy) return
    setCredentialBusy(true)
    setCredentialResult(null)
    try {
      const me = await setDeviceCredentials(devicePin, deviceRfid)
      if (!alive.current) return
      setProfile(me)
      setDevicePin('')
      setCredentialResult({ tone: 'ok', text: 'PIN perangkat tersimpan. RFID opsional juga sudah diperbarui.' })
    } catch (err) {
      if (!alive.current) return
      if (err.status === 401) return logout()
      setCredentialResult({ tone: 'bad', text: friendlyError(err) })
    } finally {
      if (alive.current) setCredentialBusy(false)
    }
  }

  /* ---------- Release ---------- */
  async function handleRelease(locker) {
    const id = locker.locker_id
    if (ops[id]?.busy) return
    const who = locker.owner?.username ? ` milik ${locker.owner.username}` : ''
    if (!window.confirm(`Lepas ${id}${who}? Locker akan kembali tersedia untuk user lain.`)) return

    patch(id, { busy: 'RELEASE', result: null })
    try {
      await releaseLocker(id)
      if (!alive.current) return
      patch(id, { busy: null, result: { title: 'Lepas locker', text: 'Locker berhasil dilepas dan kini tersedia.', tone: 'ok' } })
      await refresh()
    } catch (err) {
      if (!alive.current) return
      if (err.status === 401) return logout()
      const text = err.status === 403
        ? 'Anda bukan pemilik locker ini. Hanya pemilik atau admin yang dapat melepasnya.'
        : friendlyError(err)
      patch(id, { busy: null, result: { title: 'Lepas locker gagal', text, tone: 'bad' } })
      refresh()
    }
  }

  async function handleCycleDecision(locker, decision) {
    const id = locker.locker_id
    if (ops[id]?.busy) return

    const action = decision === 'EXTEND' ? extendLocker : finishLocker
    patch(id, { busy: decision, result: null })
    try {
      await action(id)
      if (!alive.current) return
      patch(id, {
        busy: null,
        result: {
          title: decision === 'EXTEND' ? 'Cycle diperpanjang' : 'Sesi selesai',
          text: decision === 'EXTEND'
            ? 'Cycle berikutnya siap. Masukkan barang lalu tekan LOCK.'
            : 'Locker selesai digunakan dan kini tersedia.',
          tone: 'ok',
        },
      })
      await refresh()
    } catch (err) {
      if (!alive.current) return
      if (err.status === 401) return logout()
      patch(id, {
        busy: null,
        result: {
          title: decision === 'EXTEND' ? 'Extend gagal' : 'Finish gagal',
          text: friendlyError(err),
          tone: 'bad',
        },
      })
      refresh()
    }

    async function handleResolve(locker) {
      if (!window.confirm(`Tandai ${locker.locker_id} sudah diperiksa dan lepaskan locker?`)) return
      patch(locker.locker_id, { busy: 'RESOLVE' })
      try {
        await resolveForgottenLocker(locker.locker_id)
        await Promise.all([refresh(), refreshForgotten()])
      } catch (err) {
        patch(locker.locker_id, { busy: null, result: { title: 'Penyelesaian gagal', text: friendlyError(err), tone: 'bad' } })
      }
    }
  }

  /* ---------- Command ---------- */
  async function pollCommand(lockerId, commandId, action) {
    const startedAt = Date.now()
    while (alive.current) {
      await sleep(POLL_MS)
      if (!alive.current) return
      const cmd = await getCommand(commandId)
      if (!alive.current) return
      const info = COMMAND_STATUS[cmd.status] ?? { text: String(cmd.status), tone: 'pending' }
      const finished = FINAL_STATUSES.includes(cmd.status)
      patch(lockerId, {
        busy: finished ? null : action,
        result: { title: `Perintah ${action}`, text: info.text, tone: info.tone, commandId },
      })
      if (finished) return refresh()
      if (Date.now() - startedAt > POLL_TIMEOUT_MS) {
        patch(lockerId, {
          busy: null,
          result: { title: `Perintah ${action}`, text: 'Belum ada respons dalam 30 detik. Cek status locker.', tone: 'warn', commandId },
        })
        return
      }
    }
  }

  async function handleCommand(lockerId, action) {
    if (ops[lockerId]?.busy) return
    patch(lockerId, { busy: action, result: null })
    let commandId = null
    try {
      const res = await sendLockerCommand(lockerId, action)
      commandId = res.command_id
      if (!alive.current) return
      const info = COMMAND_STATUS[res.status] ?? COMMAND_STATUS.PENDING
      patch(lockerId, { result: { title: `Perintah ${action}`, text: info.text, tone: info.tone, commandId } })
      await pollCommand(lockerId, commandId, action)
    } catch (err) {
      if (!alive.current) return
      if (err.status === 401) return logout()
      const text = err.status === 403 ? 'Anda tidak memiliki akses ke locker ini.' : friendlyError(err)
      // busy selalu dilepas agar tombol tidak stuck
      patch(lockerId, { busy: null, result: { title: `Perintah ${action} gagal`, text, tone: 'bad', commandId } })
      refresh()
    }
  }

  const isAdmin = profile?.role === 'admin'
  const hasLocker = myLockers.length > 0
  const viewer = { userId: profile?.user_id, isAdmin, hasLocker, serverNow: overview?.serverNow }
  const loading = !overview && !loadError
  const ready = Boolean(overview && profile)
  const myLockers = overview?.lockers.filter((locker) => locker.owner?.user_id === profile?.user_id) ?? []
  const availableLockers = overview?.lockers.filter((locker) => locker.availability === 'AVAILABLE') ?? []
  const adminLockers = isAdmin ? overview?.lockers ?? [] : []

  return (
    <div className="app">
      <header className="nav">
        <div className="nav__inner">
          <span className="nav__brand">
            <svg viewBox="0 0 40 40" width="28" height="28" aria-hidden="true">
              <rect width="40" height="40" rx="10" fill="#1d1d1f" />
              <g fill="none" stroke="#fff" strokeWidth="2.4" strokeLinecap="round" strokeLinejoin="round">
                <rect x="11" y="18" width="18" height="12" rx="3" />
                <path d="M15 18v-3a5 5 0 0 1 10 0v3" />
              </g>
            </svg>
            Smart Locker
          </span>
          <div className="nav__right">
            {profile && (
              <span className="nav__user">
                <span className="avatar" aria-hidden="true">{profile.username.slice(0, 1).toUpperCase()}</span>
                <span className="nav__name">{profile.username}</span>
                {isAdmin && <span className="tag tag--admin">Admin</span>}
              </span>
            )}
            <button className="btn btn--ghost" onClick={logout}>Logout</button>
          </div>
        </div>
      </header>

      <main className="content">
        <div className="page-head">
          <h1>Locker</h1>
          <p>
            {isAdmin
              ? 'Pantau status sistem dan kontrol semua locker.'
              : 'Pakai locker yang tersedia atau kelola locker milik Anda.'}
          </p>
        </div>

        {loadError && (
          <div className="alert alert--error alert--row" role="alert">
            <span>{overview ? 'Gagal memperbarui data: ' : 'Tidak dapat memuat data: '}{isAdmin ? loadError.detail : loadError.friendly}</span>
            <button className="btn btn--small" onClick={refresh}>Coba lagi</button>
          </div>
        )}

        {!isAdmin && (
          <section className="credentials" aria-labelledby="device-credentials-title">
            <div>
              <h2 id="device-credentials-title">PIN &amp; RFID perangkat</h2>
              <p>
                PIN diperlukan untuk memakai locker. PIN disimpan sebagai hash; RFID opsional.
                {profile?.device_credentials_configured
                  ? ` PIN sudah diatur${profile.device_rfid_configured ? ' dan RFID terdaftar' : ''}.`
                  : ' Atur PIN sebelum mengambil locker.'}
              </p>
            </div>
            <form onSubmit={handleCredentialsSubmit}>
              <div className="field">
                <label htmlFor="device-pin">PIN baru (4–8 digit)</label>
                <input
                  id="device-pin"
                  type="password"
                  inputMode="numeric"
                  autoComplete="new-password"
                  maxLength={8}
                  pattern="[0-9]{4,8}"
                  value={devicePin}
                  onChange={(event) => setDevicePin(event.target.value.replace(/\D/g, '').slice(0, 8))}
                  required
                />
              </div>
              <div className="field">
                <label htmlFor="device-rfid">RFID UID (opsional)</label>
                <input
                  id="device-rfid"
                  type="text"
                  autoComplete="off"
                  maxLength={20}
                  placeholder="Contoh: 04A1B2C3"
                  value={deviceRfid}
                  onChange={(event) => setDeviceRfid(event.target.value.replace(/[^0-9a-f]/gi, '').toUpperCase())}
                />
                <small className="field__hint">Kosongkan untuk memakai PIN saja. UID kartu 8, 14, atau 20 karakter heksadesimal.</small>
              </div>
              <button className="btn btn--primary" disabled={credentialBusy || devicePin.length < 4}>
                {credentialBusy ? 'Menyimpan…' : 'Simpan kredensial perangkat'}
              </button>
              {credentialResult && (
                <p className={`credentials__result credentials__result--${credentialResult.tone}`} role="status">
                  {credentialResult.text}
                </p>
              )}
            </form>
          </section>
        )}

        {isAdmin && (
          <section className="system" aria-label="Status sistem">
            <StatusItem
              label="Backend"
              value={loading ? 'Memuat…' : loadError ? 'Tidak terjangkau' : 'Terhubung'}
              tone={loading ? 'idle' : loadError ? 'bad' : 'ok'}
            />
            <StatusItem
              label="MQTT"
              value={overview?.mqttConnected === undefined ? '—' : overview.mqttConnected ? 'Terhubung' : 'Terputus'}
              tone={overview?.mqttConnected === undefined ? 'idle' : overview.mqttConnected ? 'ok' : 'bad'}
            />
            <StatusItem
              label="Device ESP32"
              value={overview?.deviceOnline === undefined ? '—' : overview.deviceOnline ? 'Online' : 'Offline'}
              tone={overview?.deviceOnline === undefined ? 'idle' : overview.deviceOnline ? 'ok' : 'bad'}
            />
          </section>
        )}

        {ready && overview.lockers.length === 0 && (
          <p className="empty">Belum ada locker yang tersedia.</p>
        )}

        {!isAdmin && <h2 className="section-title">My Locker</h2>}
        {!isAdmin && myLockers.length === 0 && ready && <p className="empty empty--compact">Belum ada locker yang Anda gunakan.</p>}
        <section className="grid" aria-label={isAdmin ? 'Daftar locker' : 'My Locker'}>
          {!ready && !loadError && (
            <>
              <div className="card skeleton" aria-hidden="true" />
              <div className="card skeleton" aria-hidden="true" />
            </>
          )}
          {ready && (isAdmin ? adminLockers : myLockers).map((locker) => (
            <LockerCard
              key={locker.locker_id}
              locker={locker}
              viewer={viewer}
              op={ops[locker.locker_id]}
              onClaim={handleClaim}
              onRelease={handleRelease}
              onCycleDecision={handleCycleDecision}
              onCommand={handleCommand}
            />
          ))}
        </section>
        {!isAdmin && <h2 className="section-title">Available Lockers</h2>}
        {!isAdmin && <section className="grid" aria-label="Available Lockers">
          {ready && availableLockers.map((locker) => (
            <LockerCard key={locker.locker_id} locker={locker} viewer={viewer} op={ops[locker.locker_id]} onClaim={handleClaim} onRelease={handleRelease} onCycleDecision={handleCycleDecision} onCommand={handleCommand} />
          ))}
        </section>}
        {isAdmin && <section className="forgotten" aria-label="Forgotten Lockers">
          <div className="section-title-row"><h2 className="section-title">Forgotten Lockers</h2><span className="tag tag--admin">{forgotten.length}</span></div>
          {forgotten.length === 0 ? <p className="empty empty--compact">Tidak ada locker yang menunggu pemeriksaan.</p> : forgotten.map((locker) => (
            <div className="forgotten__item" key={locker.locker_id}>
              <div><strong>{locker.locker_id}</strong><span>{locker.owner?.username} · batas {new Date(locker.owner.deadline_at * 1000).toLocaleString('id-ID')}</span></div>
              <button className="btn btn--primary" disabled={ops[locker.locker_id]?.busy} onClick={() => handleResolve(locker)}>Tandai diperiksa &amp; lepas</button>
            </div>
          ))}
        </section>}
      </main>
    </div>
  )
}
