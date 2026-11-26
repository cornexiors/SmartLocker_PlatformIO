import { useEffect, useState } from 'react'

const ICONS = {
  lock: <><rect x="5" y="11" width="14" height="9" rx="2" /><path d="M8 11V8a4 4 0 0 1 8 0v3" /></>,
  unlock: <><rect x="5" y="11" width="14" height="9" rx="2" /><path d="M8 11V8a4 4 0 0 1 7.5-2" /></>,
  help: <><circle cx="12" cy="12" r="9" /><path d="M9.5 9.5a2.5 2.5 0 1 1 3.5 2.3c-.7.4-1 1-1 1.7M12 17h.01" /></>,
  check: <><circle cx="12" cy="12" r="9" /><path d="m8.5 12.5 2.5 2.5 4.5-5" /></>,
  user: <><circle cx="12" cy="8" r="3.5" /><path d="M5 20a7 7 0 0 1 14 0" /></>,
  alert: <><path d="M12 4 3 20h18L12 4z" /><path d="M12 10v4M12 17h.01" /></>,
  x: <><circle cx="12" cy="12" r="9" /><path d="m9 9 6 6M15 9l-6 6" /></>,
}

function Icon({ name, size = 18 }) {
  return (
    <svg viewBox="0 0 24 24" width={size} height={size} fill="none" stroke="currentColor"
      strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true" focusable="false">
      {ICONS[name]}
    </svg>
  )
}

const Spinner = () => <span className="spinner" aria-hidden="true" />

const STATE_INFO = {
  LOCKED: { caption: 'Terkunci', icon: 'lock' },
  UNLOCKED: { caption: 'Tidak terkunci', icon: 'unlock' },
  UNKNOWN: { caption: 'Status belum diketahui', icon: 'help' },
}
const RESULT_ICON = { ok: 'check', bad: 'x', warn: 'alert' }

function formatTime(value) {
  if (value === null || value === undefined || value === '') return 'Belum ada'
  const n = typeof value === 'number' ? (value < 1e12 ? value * 1000 : value) : value
  const d = new Date(n)
  if (Number.isNaN(d.getTime())) return String(value)
  return d.toLocaleString('id-ID', { dateStyle: 'medium', timeStyle: 'short' })
}

function Deadline({ owner, serverNow }) {
  const [now, setNow] = useState(Date.now())
  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), 1000)
    return () => clearInterval(timer)
  }, [])
  if (!owner?.deadline_at) return null
  const drift = Number(serverNow) ? Number(serverNow) * 1000 - Date.now() : 0
  const remaining = Math.max(0, owner.deadline_at * 1000 - (now + drift))
  const total = Math.floor(remaining / 1000)
  const text = `${Math.floor(total / 86400)}h ${String(Math.floor(total / 3600) % 24).padStart(2, '0')}:${String(Math.floor(total / 60) % 60).padStart(2, '0')}:${String(total % 60).padStart(2, '0')}`
  const forgotten = owner.warning_status === 'FORGOTTEN' || remaining === 0
  return (
    <div className={`deadline deadline--${forgotten ? 'bad' : 'warn'}`}>
      <strong>{forgotten ? 'Deadline terlewati — menunggu pemeriksaan admin' : 'Peringatan penyimpanan'}</strong>
      <span>{forgotten ? 'Barang tetap menjadi tanggung jawab pemilik.' : `Sisa waktu: ${text}`}</span>
      <small>Batas: {formatTime(owner.deadline_at)}</small>
    </div>
  )
}

// viewer: { userId, isAdmin }. Tombol hanya UI; backend tetap memvalidasi hak akses.
export default function LockerCard({ locker, viewer, op = {}, onClaim, onRelease, onCycleDecision, onCommand }) {
  const id = locker.locker_id
  const state = locker.state === 'LOCKED' || locker.state === 'UNLOCKED' ? locker.state : 'UNKNOWN'
  const info = STATE_INFO[state]
  const online = Boolean(locker.online)
  const owner = locker.owner
  const occupied = locker.availability ? locker.availability === 'OCCUPIED' : Boolean(owner)
  const isOwner = occupied && owner?.user_id === viewer.userId

  const forgotten = owner?.warning_status === 'FORGOTTEN'
  const canControl = viewer.isAdmin || (isOwner && !forgotten)
  const waitingForDecision = isOwner && owner?.occupancy_state === 'EXTENSION_PENDING'
  const canClaim = !viewer.isAdmin && !occupied && !viewer.hasLocker
  const canRelease = occupied && (viewer.isAdmin || isOwner) && !waitingForDecision && owner?.warning_status !== 'FORGOTTEN'
  const busy = Boolean(op.busy)
  const result = op.result
  const ownerName = occupied ? `${owner?.username ?? 'Tidak diketahui'}${isOwner ? ' (Anda)' : ''}` : 'Belum ada'

  return (
    <article className={`card card--${state.toLowerCase()}`} aria-labelledby={`${id}-title`} aria-busy={busy}>
      <header className="card__head">
        <h2 className="card__title" id={`${id}-title`}>{id}</h2>
        <span className={`pill pill--${occupied ? 'occupied' : 'available'}`}>
          <Icon name={occupied ? 'user' : 'check'} size={14} />
          {occupied ? 'OCCUPIED' : 'AVAILABLE'}
        </span>
      </header>

      <div className="card__state" aria-live="polite">
        <span className={`state-badge state-badge--${state.toLowerCase()}`}>
          <Icon name={info.icon} size={28} />
        </span>
        <div>
          <div className="state-label">{state}</div>
          <div className="state-caption">{info.caption}</div>
        </div>
      </div>

      <dl className="details">
        {viewer.isAdmin && <div>
          <dt>Koneksi</dt>
          <dd className={`conn conn--${online ? 'on' : 'off'}`}><span className="conn__dot" aria-hidden="true" />{online ? 'Online' : 'Offline'}</dd>
        </div>}
        <div>
          <dt>Pemilik</dt>
          <dd>{ownerName}</dd>
        </div>
        {occupied && owner?.assigned_at && (
          <div>
            <dt>Dipakai sejak</dt>
            <dd>{formatTime(owner.assigned_at)}</dd>
          </div>
        )}
        {viewer.isAdmin && <div>
          <dt>Konfirmasi terakhir</dt>
          <dd>{formatTime(locker.last_ack)}</dd>
        </div>}
      </dl>
      {isOwner && <Deadline owner={owner} serverNow={viewer.serverNow} />}

      {(canClaim || canControl || canRelease) ? (
        <div className="actions">
          {canClaim && (
            <button className="btn btn--primary btn--block" disabled={busy} onClick={() => onClaim(locker)}>
              {op.busy === 'CLAIM' ? <><Spinner />Memproses…</> : 'Pakai Locker'}
            </button>
          )}

          {canControl && !waitingForDecision && (
            <div className="actions__row">
              <button className="btn btn--secondary" disabled={busy} onClick={() => onCommand(id, 'LOCK')}>
                {op.busy === 'LOCK' ? <><Spinner />Mengirim…</> : <><Icon name="lock" size={16} />LOCK</>}
              </button>
              <button className="btn btn--secondary" disabled={busy} onClick={() => onCommand(id, 'UNLOCK')}>
                {op.busy === 'UNLOCK' ? <><Spinner />Mengirim…</> : <><Icon name="unlock" size={16} />UNLOCK</>}
              </button>
            </div>
          )}

          {waitingForDecision && (
            <>
              <p className="note"><Icon name="help" size={16} />Cycle selesai. Mau menggunakan locker lagi?</p>
              <div className="actions__row">
                <button className="btn btn--primary" disabled={busy} onClick={() => onCycleDecision(id, 'EXTEND')}>
                  {op.busy === 'EXTEND' ? <><Spinner />Memproses…</> : 'Extend lagi'}
                </button>
                <button className="btn btn--secondary" disabled={busy} onClick={() => onCycleDecision(id, 'FINISH')}>
                  {op.busy === 'FINISH' ? <><Spinner />Memproses…</> : 'Finish'}
                </button>
              </div>
            </>
          )}

          {canRelease && (
            <button className="btn btn--link btn--block" disabled={busy} onClick={() => onRelease(locker)}>
              {op.busy === 'RELEASE' ? <><Spinner />Memproses…</> : 'Lepas Locker'}
            </button>
          )}
        </div>
      ) : (
        occupied
          ? <p className="note"><Icon name={forgotten && isOwner ? 'alert' : 'user'} size={16} />{forgotten && isOwner ? 'Locker menunggu pemeriksaan admin.' : `Sedang dipakai oleh ${owner?.username ?? 'user lain'}.`}</p>
          : viewer.hasLocker && !viewer.isAdmin
            ? <p className="note"><Icon name="user" size={16} />Akun hanya dapat menggunakan satu locker dalam satu waktu.</p>
            : null
      )}

      {result && (
        <div className={`result result--${result.tone}`} role={result.tone === 'bad' ? 'alert' : 'status'}>
          <span className="result__icon">
            {result.tone === 'pending' ? <Spinner /> : <Icon name={RESULT_ICON[result.tone] ?? 'check'} size={18} />}
          </span>
          <div>
            <strong>{result.title}</strong>
            <p>{result.text}</p>
            {result.commandId && <small>ID: {result.commandId}</small>}
          </div>
        </div>
      )}
    </article>
  )
}
