// tone: 'ok' | 'bad' | 'idle' — makna status selalu ada di teks, bukan hanya warna.
export default function StatusItem({ label, value, tone = 'idle' }) {
  return (
    <div className="sys-item">
      <span className="sys-item__label">{label}</span>
      <span className={`sys-item__value sys-item__value--${tone}`}>
        <span className="sys-dot" aria-hidden="true" />
        {value}
      </span>
    </div>
  )
}
