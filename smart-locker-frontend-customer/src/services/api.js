// Semua komunikasi ke backend ada di file ini.
// VITE_API_URL kosong/tidak diisi => request relatif (memakai proxy Vite).
const API_URL = (import.meta.env.VITE_API_URL ?? '').replace(/\/$/, '')

const TOKEN_KEY = 'access_token'
const EXPIRES_KEY = 'access_token_expires_at'
const PROFILE_KEY = 'user_profile'

export class ApiError extends Error {
  constructor(message, status = 0) {
    super(message)
    this.name = 'ApiError'
    this.status = status
  }
}

/* ---------- Sesi (sessionStorage) ---------- */

export function getToken() {
  try { return sessionStorage.getItem(TOKEN_KEY) } catch { return null }
}

export function getTokenExpiry() {
  try { return Number(sessionStorage.getItem(EXPIRES_KEY)) || 0 } catch { return 0 }
}

export function getProfile() {
  try { return JSON.parse(sessionStorage.getItem(PROFILE_KEY)) } catch { return null }
}

function saveSession(token, expiresIn) {
  sessionStorage.setItem(TOKEN_KEY, token)
  sessionStorage.setItem(EXPIRES_KEY, String(Date.now() + (Number(expiresIn) || 3600) * 1000))
}

// Hapus token, waktu kedaluwarsa, dan profil user
export function clearSession() {
  try {
    sessionStorage.removeItem(TOKEN_KEY)
    sessionStorage.removeItem(EXPIRES_KEY)
    sessionStorage.removeItem(PROFILE_KEY)
  } catch { /* abaikan */ }
}

export function isAuthenticated() {
  if (!getToken()) return false
  const exp = getTokenExpiry()
  if (exp && Date.now() >= exp) {
    clearSession()
    return false
  }
  return true
}

/* ---------- Request helper ---------- */

function messageFrom(data, status) {
  const m = data?.detail ?? data?.error ?? data?.message
  if (typeof m === 'string') return m
  if (Array.isArray(m)) return m.map((x) => x?.msg ?? JSON.stringify(x)).join('. ')
  if (m) return JSON.stringify(m)
  return `Permintaan gagal (HTTP ${status})`
}

async function request(path, { method = 'GET', body, auth = true } = {}) {
  const headers = { Accept: 'application/json' }
  if (body !== undefined) headers['Content-Type'] = 'application/json'

  if (auth) {
    const token = getToken()
    if (!token) throw new ApiError('Sesi berakhir. Silakan masuk lagi.', 401)
    headers.Authorization = `Bearer ${token}`
  }

  let res
  try {
    res = await fetch(`${API_URL}${path}`, {
      method,
      headers,
      body: body !== undefined ? JSON.stringify(body) : undefined,
    })
  } catch {
    throw new ApiError('Tidak dapat terhubung ke backend. Pastikan server berjalan di alamat yang benar.', 0)
  }

  const data = await res.json().catch(() => null)

  if (!res.ok) {
    if (res.status === 401 && auth) {
      clearSession()
      throw new ApiError('Sesi berakhir. Silakan masuk lagi.', 401)
    }
    throw new ApiError(messageFrom(data, res.status), res.status)
  }
  return data
}

/* ---------- Auth ---------- */

export async function requestRegistrationOtp(username, email, password) {
  try {
    return await request('/auth/register/request-otp', {
      method: 'POST', body: { username, email, password }, auth: false,
    })
  } catch (err) {
    if (err.status === 409) throw new ApiError('Username atau email sudah terdaftar.', 409)
    throw err
  }
}

export async function verifyRegistrationOtp(username, email, otp) {
  return request('/auth/register/verify-otp', {
    method: 'POST', body: { username, email, otp }, auth: false,
  })
}

// Login lalu langsung ambil profil lewat GET /me. Mengembalikan profil user.
export async function login(username, password) {
  let data
  try {
    data = await request('/auth/login', { method: 'POST', body: { username, password }, auth: false })
  } catch (err) {
    if (err.status === 401) throw new ApiError('Username atau password salah.', 401)
    throw err
  }
  if (!data?.access_token) throw new ApiError('Respons login tidak valid dari server.', 500)
  saveSession(data.access_token, data.expires_in)

  try {
    return await getMe()
  } catch (err) {
    clearSession()
    throw err
  }
}

export async function getMe() {
  const me = await request('/me')
  if (!me?.username) throw new ApiError('Respons profil tidak valid dari server.', 500)
  sessionStorage.setItem(PROFILE_KEY, JSON.stringify(me))
  return me
}

export async function setDeviceCredentials(pin, rfidUid) {
  const result = await request('/me/device-credentials', {
    method: 'PUT',
    body: { pin, rfid_uid: rfidUid },
  })
  const profile = { ...getProfile(), ...result }
  sessionStorage.setItem(PROFILE_KEY, JSON.stringify(profile))
  return profile
}

export function logout() {
  clearSession()
}

/* ---------- Locker ---------- */

// Menormalkan respons /lockers: daftar locker bisa berupa array atau objek.
export async function getLockers() {
  const data = await request('/lockers')
  const raw = Array.isArray(data) ? data : data?.lockers
  const list = Array.isArray(raw)
    ? raw
    : Object.entries(raw ?? {}).map(([id, l]) => ({ locker_id: id, ...l }))
  list.sort((a, b) => String(a.locker_id).localeCompare(String(b.locker_id)))
  return {
    mqttConnected: Array.isArray(data) ? undefined : data?.mqtt_connected,
    deviceOnline: Array.isArray(data) ? undefined : data?.device_online,
    serverNow: Array.isArray(data) ? undefined : data?.server_now,
    timeZone: Array.isArray(data) ? undefined : data?.time_zone,
    lockers: list,
  }
}

export const getForgottenLockers = () => request('/admin/forgotten-lockers')

export const resolveForgottenLocker = (lockerId) =>
  request(`/admin/forgotten-lockers/${encodeURIComponent(lockerId)}/resolve`, { method: 'POST' })

export const getLockerStatus = (lockerId) =>
  request(`/lockers/${encodeURIComponent(lockerId)}/status`)

export const claimLocker = (lockerId) =>
  request(`/lockers/${encodeURIComponent(lockerId)}/claim`, { method: 'POST' })

export const releaseLocker = (lockerId) =>
  request(`/lockers/${encodeURIComponent(lockerId)}/release`, { method: 'POST' })

export const extendLocker = (lockerId) =>
  request(`/lockers/${encodeURIComponent(lockerId)}/extend`, { method: 'POST' })

export const finishLocker = (lockerId) =>
  request(`/lockers/${encodeURIComponent(lockerId)}/finish`, { method: 'POST' })

export const sendLockerCommand = (lockerId, action) =>
  request(`/lockers/${encodeURIComponent(lockerId)}/command`, { method: 'POST', body: { action } })

export const getCommand = (commandId) =>
  request(`/commands/${encodeURIComponent(commandId)}`)
