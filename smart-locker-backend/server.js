import crypto from "node:crypto";
import path from "node:path";
import { fileURLToPath } from "node:url";

import express from "express";
import mqtt from "mqtt";
import Database from "better-sqlite3";

import cors from "cors";
import swaggerUi from "swagger-ui-express";

// ============================================================
// CONFIGURATION
// ============================================================

const NODE_ENV = process.env.NODE_ENV ?? "development";
const IS_PRODUCTION = NODE_ENV === "production";
const MQTT_HOST = process.env.MQTT_HOST ?? "broker.hivemq.com";
const MQTT_PORT = Number(process.env.MQTT_PORT ?? 8883);
const MQTT_PROTOCOL = process.env.MQTT_PROTOCOL ?? "mqtts";
const MQTT_USERNAME = process.env.MQTT_USERNAME;
const MQTT_PASSWORD = process.env.MQTT_PASSWORD;
const TOPIC_PREFIX = "smartlocker/demo";

const HMAC_SECRET_VALUE = process.env.SMARTLOCKER_HMAC_SECRET;
const JWT_SECRET_VALUE = process.env.SMARTLOCKER_JWT_SECRET;
const ADMIN_USERNAME = process.env.SMARTLOCKER_ADMIN_USERNAME;
const ADMIN_PASSWORD = process.env.SMARTLOCKER_ADMIN_PASSWORD;
const CORS_ORIGINS = (
  process.env.CORS_ORIGINS ??
  "http://localhost:5173,http://127.0.0.1:5173,http://localhost:5174,http://127.0.0.1:5174,http://localhost:3000,http://localhost:8000,http://127.0.0.1:8000"
)
  .split(",")
  .map((origin) => origin.trim())
  .filter(Boolean);

if (IS_PRODUCTION) {
  const requiredProductionConfig = {
    SMARTLOCKER_HMAC_SECRET: HMAC_SECRET_VALUE,
    SMARTLOCKER_JWT_SECRET: JWT_SECRET_VALUE,
    SMARTLOCKER_ADMIN_USERNAME: ADMIN_USERNAME,
    SMARTLOCKER_ADMIN_PASSWORD: ADMIN_PASSWORD,
    MQTT_USERNAME,
    MQTT_PASSWORD,
  };

  const missingConfig = Object.entries(requiredProductionConfig)
    .filter(([, value]) => !value)
    .map(([name]) => name);

  if (missingConfig.length > 0) {
    throw new Error(
      `Missing required production environment variables: ${missingConfig.join(", ")}`,
    );
  }

  if (HMAC_SECRET_VALUE.length < 32 || JWT_SECRET_VALUE.length < 32) {
    throw new Error(
      "Production HMAC and JWT secrets must each be at least 32 characters.",
    );
  }

  if (MQTT_PROTOCOL !== "mqtts" || MQTT_PORT !== 8883) {
    throw new Error("Production MQTT must use mqtts on TLS port 8883.");
  }
}

const HMAC_SECRET = Buffer.from(
  HMAC_SECRET_VALUE ?? "smart-locker-demo-secret-key-32",
  "utf8",
);
const JWT_SECRET = Buffer.from(
  JWT_SECRET_VALUE ?? "development-only-jwt-secret-change-me",
  "utf8",
);
const AUTH_USERNAME = ADMIN_USERNAME ?? "admin";
const AUTH_PASSWORD = ADMIN_PASSWORD ?? "change-me";
const loginAttempts = new Map();
const LOGIN_WINDOW_MS = 15 * 60 * 1000;
const MAX_LOGIN_FAILURES = 5;
const BREVO_API_KEY = process.env.BREVO_API_KEY;
const EMAIL_FROM = process.env.EMAIL_FROM;
const EMAIL_FROM_NAME = process.env.EMAIL_FROM_NAME ?? "Smart Locker";
const BREVO_API_URL =
  process.env.BREVO_API_URL ?? "https://api.brevo.com/v3/smtp/email";
const otpRequestLimits = new Map();
const OTP_EXPIRY_SECONDS = 5 * 60;
const OTP_RESEND_COOLDOWN_MS = 60 * 1000;
const OTP_MAX_ATTEMPTS = 5;
const OTP_WINDOW_MS = 15 * 60 * 1000;
const OTP_MAX_REQUESTS_PER_WINDOW = 5;

const COMMAND_ACK_TIMEOUT = 15; // seconds
const CAMPUS_TIME_ZONE = "Asia/Jakarta";
const REMINDER_HOUR = 17;
const MAINTENANCE_INTERVAL_MS = 30 * 1000;

const HTTP_PORT = Number(process.env.PORT ?? 8000);
const HTTP_HOST = process.env.HOST ?? "127.0.0.1";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const DATABASE_PATH =
  process.env.DATABASE_PATH ?? path.join(__dirname, "smartlocker.db");

// ============================================================
// STATE
// (Node itu single-threaded, jadi lock ala Python nggak diperlukan.)
// ============================================================

let DEVICE_ONLINE = false;
let mqttConnected = false;

const LOCKERS = {
  LOCKER_001: { state: "UNKNOWN", online: false, last_ack: null },
  LOCKER_002: { state: "UNKNOWN", online: false, last_ack: null },
  LOCKER_003: { state: "UNKNOWN", online: false, last_ack: null },
  LOCKER_004: { state: "UNKNOWN", online: false, last_ack: null },
};

/** @type {Map<string, Record<string, any>>} */
const COMMANDS = new Map();

// ============================================================
// HELPERS
// ============================================================

const nowSeconds = () => Date.now() / 1000; // setara time.time()

function jakartaDateParts(epochSeconds = nowSeconds()) {
  const parts = new Intl.DateTimeFormat("en-CA", {
    timeZone: CAMPUS_TIME_ZONE,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    hourCycle: "h23",
  }).formatToParts(new Date(epochSeconds * 1000));
  return Object.fromEntries(parts.filter((part) => part.type !== "literal").map((part) => [part.type, part.value]));
}

function jakartaDate(epochSeconds = nowSeconds()) {
  const p = jakartaDateParts(epochSeconds);
  return `${p.year}-${p.month}-${p.day}`;
}

function addCalendarDays(dateText, days) {
  const date = new Date(`${dateText}T00:00:00Z`);
  date.setUTCDate(date.getUTCDate() + days);
  return date.toISOString().slice(0, 10);
}

function jakartaMidnight(dateText) {
  // Asia/Jakarta is UTC+07:00 and has no DST transitions.
  return Date.parse(`${dateText}T00:00:00+07:00`) / 1000;
}

const lockerTopic = (lockerId, suffix) =>
  `${TOPIC_PREFIX}/${lockerId}/${suffix}`;

const hasLocker = (lockerId) =>
  Object.prototype.hasOwnProperty.call(LOCKERS, lockerId);

const base64UrlEncode = (value) =>
  Buffer.from(value).toString("base64url");

const base64UrlDecode = (value) =>
  Buffer.from(value, "base64url").toString("utf8");

const secureEqual = (left, right) => {
  const leftBuffer = Buffer.from(left);
  const rightBuffer = Buffer.from(right);

  return (
    leftBuffer.length === rightBuffer.length &&
    crypto.timingSafeEqual(leftBuffer, rightBuffer)
  );
};

function createAccessToken(user) {
  const header = base64UrlEncode(JSON.stringify({ alg: "HS256", typ: "JWT" }));
  const payload = base64UrlEncode(
    JSON.stringify({
      sub: user.username,
      user_id: user.id,
      role: user.role,
      iat: Math.floor(Date.now() / 1000),
      exp: Math.floor(Date.now() / 1000) + 3600,
    }),
  );
  const content = `${header}.${payload}`;
  const signature = crypto
    .createHmac("sha256", JWT_SECRET)
    .update(content)
    .digest("base64url");

  return `${content}.${signature}`;
}

function hashPassword(password) {
  const salt = crypto.randomBytes(16).toString("hex");
  const hash = crypto.scryptSync(password, salt, 64).toString("hex");
  return `scrypt$${salt}$${hash}`;
}

function verifyPassword(password, storedHash) {
  const [algorithm, salt, expectedHash] = storedHash.split("$");
  if (algorithm !== "scrypt" || !salt || !expectedHash) return false;

  const actualHash = crypto.scryptSync(password, salt, 64).toString("hex");
  return secureEqual(actualHash, expectedHash);
}

function normalizeEmail(email) {
  return email.trim().toLowerCase();
}

function hashOtp(otp) {
  return crypto.createHash("sha256").update(otp, "utf8").digest("hex");
}

function isValidEmail(email) {
  return email.length <= 254 &&
    /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email);
}

function checkOtpRateLimit(email, ip) {
  const currentTime = Date.now();
  const keys = [`email:${email}`, `ip:${ip}`];
  let limited = false;
  for (const key of keys) {
    const current = otpRequestLimits.get(key);
    if (!current || currentTime - current.startedAt >= OTP_WINDOW_MS) {
      otpRequestLimits.set(key, { startedAt: currentTime, count: 1 });
    } else if (current.count >= OTP_MAX_REQUESTS_PER_WINDOW) {
      limited = true;
    } else {
      current.count += 1;
    }
  }
  return limited;
}

async function sendRegistrationOtp(email, otp) {
  if (!BREVO_API_KEY || !EMAIL_FROM) {
    const error = new Error("email_not_configured");
    error.code = "EMAIL_NOT_CONFIGURED";
    throw error;
  }

  const response = await fetch(BREVO_API_URL, {
    method: "POST",
    headers: {
      accept: "application/json",
      "api-key": BREVO_API_KEY,
      "content-type": "application/json",
    },
    body: JSON.stringify({
      sender: { email: EMAIL_FROM, name: EMAIL_FROM_NAME },
      to: [{ email }],
      subject: "Your Smart Locker verification code",
      htmlContent: `<p>Your Smart Locker verification code is <strong>${otp}</strong>.</p><p>This code expires in 5 minutes.</p>`,
    }),
    signal: AbortSignal.timeout(10_000),
  });

  if (!response.ok) {
    const error = new Error("email_delivery_failed");
    error.code = "EMAIL_DELIVERY_FAILED";
    throw error;
  }
}

function verifyAccessToken(token) {
  const parts = token.split(".");
  if (parts.length !== 3) return null;

  const [header, payload, signature] = parts;
  let decodedHeader;
  let decodedPayload;

  try {
    decodedHeader = JSON.parse(base64UrlDecode(header));
    decodedPayload = JSON.parse(base64UrlDecode(payload));
  } catch {
    return null;
  }

  if (decodedHeader.alg !== "HS256" || decodedHeader.typ !== "JWT") {
    return null;
  }

  const expectedSignature = crypto
    .createHmac("sha256", JWT_SECRET)
    .update(`${header}.${payload}`)
    .digest("base64url");
  if (!secureEqual(signature, expectedSignature)) {
    return null;
  }

  if (
    typeof decodedPayload.sub !== "string" ||
    typeof decodedPayload.exp !== "number" ||
    decodedPayload.exp <= Math.floor(Date.now() / 1000)
  ) {
    return null;
  }

  return decodedPayload;
}

function requireAuthentication(req, res, next) {
  const authorization = req.get("authorization") ?? "";
  const [scheme, token] = authorization.split(" ");

  if (scheme !== "Bearer" || !token) {
    return httpError(res, 401, "Bearer token is required");
  }

  const claims = verifyAccessToken(token);
  if (!claims) return httpError(res, 401, "Invalid or expired token");

  const user = db
    .prepare("SELECT id, username, role FROM users WHERE id = ?")
    .get(claims.user_id);

  if (!user) return httpError(res, 401, "User account is no longer available");

  req.auth = user;
  return next();
}

// ============================================================
// HMAC COMMAND SIGNING
// ============================================================

function generateCommand(lockerId, action, commandId) {
  const timestamp = Math.floor(Date.now() / 1000);
  const nonce = crypto.randomBytes(4).toString("hex").toUpperCase();
  const message = `${lockerId}|${action}|${timestamp}|${nonce}|${commandId}`;

  const signature = crypto
    .createHmac("sha256", HMAC_SECRET)
    .update(message, "utf8")
    .digest("hex");

  return `${message}|${signature}`;
}

function generateSignedUserSync(fields) {
  const timestamp = Math.floor(Date.now() / 1000);
  const nonce = crypto.randomBytes(4).toString("hex").toUpperCase();
  const message = [...fields, timestamp, nonce].join("|");
  const signature = crypto
    .createHmac("sha256", HMAC_SECRET)
    .update(message, "utf8")
    .digest("hex");
  return `${message}|${signature}`;
}

// ============================================================
// DATABASE
// ============================================================

const db = new Database(DATABASE_PATH);

function initializeDatabase() {
  db.pragma("journal_mode = WAL");
  db.pragma("foreign_keys = ON");

  db.exec(`
    CREATE TABLE IF NOT EXISTS users (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      username TEXT NOT NULL UNIQUE COLLATE NOCASE,
      email TEXT COLLATE NOCASE,
      password_hash TEXT NOT NULL,
      role TEXT NOT NULL CHECK (role IN ('admin', 'user')),
      device_rfid_uid TEXT,
      device_pin_salt TEXT,
      device_pin_hash TEXT,
      created_at REAL NOT NULL
    );

    CREATE TABLE IF NOT EXISTS locker_ownership (
      locker_id TEXT PRIMARY KEY,
      user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      assigned_at REAL NOT NULL,
      unlock_count INTEGER NOT NULL DEFAULT 0,
      occupancy_state TEXT NOT NULL DEFAULT 'RESERVED',
      reminder_date TEXT,
      deadline_at REAL,
      warning_status TEXT NOT NULL DEFAULT 'NONE',
      forgotten_at REAL,
      checked_by INTEGER REFERENCES users(id),
      checked_at REAL
    );

    CREATE INDEX IF NOT EXISTS idx_locker_ownership_user
      ON locker_ownership(user_id);

    CREATE TABLE IF NOT EXISTS commands (
      command_id TEXT PRIMARY KEY,
      locker_id TEXT NOT NULL,
      action TEXT NOT NULL,
      status TEXT NOT NULL,
      mqtt_topic TEXT NOT NULL,
      signed_payload TEXT NOT NULL,
      created_at REAL NOT NULL,
      updated_at REAL NOT NULL,
      retry_count INTEGER NOT NULL DEFAULT 0,
      last_attempt_at REAL,
      acknowledged_at REAL,
      acknowledgment TEXT,
      result TEXT,
      last_error TEXT,
      requested_by_user_id INTEGER,
      requested_by_role TEXT
    );

    CREATE INDEX IF NOT EXISTS idx_commands_locker_created
      ON commands(locker_id, created_at);

    CREATE INDEX IF NOT EXISTS idx_commands_status
      ON commands(status);

    CREATE TABLE IF NOT EXISTS forgotten_locker_resolutions (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      locker_id TEXT NOT NULL,
      user_id INTEGER NOT NULL,
      checked_by INTEGER NOT NULL REFERENCES users(id),
      checked_at REAL NOT NULL,
      released_at REAL NOT NULL
    );

    CREATE TABLE IF NOT EXISTS pending_registrations (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      username TEXT NOT NULL UNIQUE COLLATE NOCASE,
      email TEXT NOT NULL UNIQUE COLLATE NOCASE,
      password_hash TEXT NOT NULL,
      otp_hash TEXT NOT NULL,
      otp_expires_at REAL NOT NULL,
      otp_attempts INTEGER NOT NULL DEFAULT 0,
      last_sent_at REAL NOT NULL,
      created_at REAL NOT NULL,
      request_ip TEXT
    );

    CREATE TABLE IF NOT EXISTS device_events (
      event_id TEXT PRIMARY KEY,
      received_at REAL NOT NULL
    );
  `);

  const addColumnIfMissing = (table, column, definition) => {
    const columns = db.prepare(`PRAGMA table_info(${table})`).all();
    if (!columns.some((entry) => entry.name === column)) {
      db.exec(`ALTER TABLE ${table} ADD COLUMN ${column} ${definition}`);
    }
  };

  addColumnIfMissing(
    "locker_ownership",
    "unlock_count",
    "INTEGER NOT NULL DEFAULT 0",
  );
  addColumnIfMissing(
    "locker_ownership",
    "occupancy_state",
    "TEXT NOT NULL DEFAULT 'RESERVED'",
  );
  addColumnIfMissing("users", "device_rfid_uid", "TEXT");
  addColumnIfMissing("users", "device_pin_salt", "TEXT");
  addColumnIfMissing("users", "device_pin_hash", "TEXT");
  db.exec(`
    CREATE UNIQUE INDEX IF NOT EXISTS idx_users_device_rfid_uid
      ON users(device_rfid_uid)
      WHERE device_rfid_uid IS NOT NULL AND device_rfid_uid <> '';
  `);
  addColumnIfMissing("commands", "requested_by_user_id", "INTEGER");
  addColumnIfMissing("commands", "requested_by_role", "TEXT");
  addColumnIfMissing("locker_ownership", "reminder_date", "TEXT");
  addColumnIfMissing("locker_ownership", "deadline_at", "REAL");
  addColumnIfMissing(
    "locker_ownership",
    "warning_status",
    "TEXT NOT NULL DEFAULT 'NONE'",
  );
  addColumnIfMissing("locker_ownership", "forgotten_at", "REAL");
  addColumnIfMissing("locker_ownership", "checked_by", "INTEGER");
  addColumnIfMissing("locker_ownership", "checked_at", "REAL");
  addColumnIfMissing("users", "email", "TEXT");
  db.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_users_email ON users(email) WHERE email IS NOT NULL");

  const admin = db
    .prepare("SELECT id FROM users WHERE username = ?")
    .get(AUTH_USERNAME);

  if (!admin) {
    db.prepare(
      `INSERT INTO users (username, password_hash, role, created_at)
       VALUES (?, ?, 'admin', ?)`,
    ).run(AUTH_USERNAME, hashPassword(AUTH_PASSWORD), nowSeconds());
  }

  const duplicateOwners = db
    .prepare(
      `SELECT user_id
       FROM locker_ownership
       GROUP BY user_id
       HAVING COUNT(*) > 1`,
    )
    .all();
  if (duplicateOwners.length === 0) {
    db.exec(
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_locker_ownership_one_per_user ON locker_ownership(user_id)",
    );
  } else {
    console.warn(
      `Found ${duplicateOwners.length} existing user(s) with multiple lockers. Their assignments were preserved; admin review is required.`,
    );
  }
  db.exec(`
    CREATE TRIGGER IF NOT EXISTS trg_locker_ownership_one_per_user
    BEFORE INSERT ON locker_ownership
    WHEN EXISTS (
      SELECT 1 FROM locker_ownership WHERE user_id = NEW.user_id
    )
    BEGIN
      SELECT RAISE(ABORT, 'user_already_has_locker');
    END;
  `);

  console.log("Command database initialized.");
}

const upsertCommandStmt = () =>
  db.prepare(`
    INSERT INTO commands (
      command_id, locker_id, action, status, mqtt_topic, signed_payload,
      created_at, updated_at, retry_count, last_attempt_at,
      acknowledged_at, acknowledgment, result, last_error
      , requested_by_user_id, requested_by_role
    )
    VALUES (
      @command_id, @locker_id, @action, @status, @mqtt_topic, @signed_payload,
      @created_at, @updated_at, @retry_count, @last_attempt_at,
      @acknowledged_at, @acknowledgment, @result, @last_error,
      @requested_by_user_id, @requested_by_role
    )
    ON CONFLICT(command_id) DO UPDATE SET
      locker_id = excluded.locker_id,
      action = excluded.action,
      status = excluded.status,
      mqtt_topic = excluded.mqtt_topic,
      signed_payload = excluded.signed_payload,
      created_at = excluded.created_at,
      updated_at = excluded.updated_at,
      retry_count = excluded.retry_count,
      last_attempt_at = excluded.last_attempt_at,
      acknowledged_at = excluded.acknowledged_at,
      acknowledgment = excluded.acknowledgment,
      result = excluded.result,
      last_error = excluded.last_error,
      requested_by_user_id = excluded.requested_by_user_id,
      requested_by_role = excluded.requested_by_role
  `);

let _upsert = null;

function persistCommand(command) {
  _upsert ??= upsertCommandStmt();
  _upsert.run({
    command_id: command.command_id,
    locker_id: command.locker_id,
    action: command.action,
    status: command.status,
    mqtt_topic: command.topic,
    signed_payload: command.payload,
    created_at: command.created_at,
    updated_at: command.updated_at ?? command.created_at,
    retry_count: command.retry_count ?? 0,
    last_attempt_at: command.last_attempt_at ?? null,
    acknowledged_at: command.acknowledged_at ?? null,
    acknowledgment: command.acknowledgment ?? null,
    result: command.result ?? null,
    last_error: command.last_error ?? null,
    requested_by_user_id: command.requested_by_user_id ?? null,
    requested_by_role: command.requested_by_role ?? null,
  });
}

function loadCommandsFromDatabase() {
  const rows = db.prepare("SELECT * FROM commands").all();

  COMMANDS.clear();
  for (const row of rows) {
    COMMANDS.set(row.command_id, {
      command_id: row.command_id,
      locker_id: row.locker_id,
      action: row.action,
      status: row.status,
      topic: row.mqtt_topic,
      payload: row.signed_payload,
      created_at: row.created_at,
      updated_at: row.updated_at,
      retry_count: row.retry_count,
      last_attempt_at: row.last_attempt_at,
      acknowledged_at: row.acknowledged_at,
      acknowledgment: row.acknowledgment,
      result: row.result,
      last_error: row.last_error,
      requested_by_user_id: row.requested_by_user_id,
      requested_by_role: row.requested_by_role,
    });
  }

  console.log(`Restored ${COMMANDS.size} command(s) from database.`);
}

function updateCommand(commandId, changes) {
  const command = COMMANDS.get(commandId);
  if (!command) return null;

  Object.assign(command, changes);
  command.updated_at = nowSeconds();
  persistCommand(command);
  return { ...command };
}

function getLockerOwner(lockerId) {
  return db
    .prepare(
      `SELECT u.id, u.username, u.role, o.assigned_at,
              o.unlock_count, o.occupancy_state
              , o.reminder_date, o.deadline_at, o.warning_status,
              o.forgotten_at, o.checked_by, o.checked_at
       FROM locker_ownership o
       JOIN users u ON u.id = o.user_id
       WHERE o.locker_id = ?`,
    )
    .get(lockerId) ?? null;
}

function getLockerView(lockerId, viewer = null) {
  const owner = getLockerOwner(lockerId);
  const canSeeOwner = owner && (
    viewer?.role === "admin" || viewer?.id === owner.id
  );

  return {
    locker_id: lockerId,
    availability: owner ? "OCCUPIED" : "AVAILABLE",
    owner: canSeeOwner
      ? {
          user_id: owner.id,
          username: owner.username,
          assigned_at: owner.assigned_at,
          unlock_count: owner.unlock_count,
          occupancy_state: owner.occupancy_state,
          reminder_date: owner.reminder_date,
          deadline_at: owner.deadline_at,
          warning_status: owner.warning_status,
          forgotten_at: viewer?.role === "admin" ? owner.forgotten_at : undefined,
          checked_by: viewer?.role === "admin" ? owner.checked_by : undefined,
          checked_at: viewer?.role === "admin" ? owner.checked_at : undefined,
        }
      : null,
    state: LOCKERS[lockerId].state,
    ...(viewer?.role === "admin"
      ? { online: LOCKERS[lockerId].online, last_ack: LOCKERS[lockerId].last_ack }
      : {}),
  };
}

function runLockerMaintenance(epoch = nowSeconds()) {
  const today = jakartaDate(epoch);
  const currentHour = Number(jakartaDateParts(epoch).hour);
  const rows = db.prepare(
    `SELECT locker_id, user_id, occupancy_state, reminder_date, deadline_at,
            warning_status
     FROM locker_ownership
     WHERE occupancy_state IN ('OCCUPIED', 'EXTENSION_PENDING')`,
  ).all();

  const updateWarning = db.prepare(
    `UPDATE locker_ownership
     SET reminder_date = ?, deadline_at = ?, warning_status = 'WARNED'
     WHERE locker_id = ? AND warning_status = 'NONE'
       AND occupancy_state IN ('OCCUPIED', 'EXTENSION_PENDING')`,
  );
  const markForgotten = db.prepare(
    `UPDATE locker_ownership
     SET warning_status = 'FORGOTTEN', forgotten_at = ?
     WHERE locker_id = ? AND warning_status = 'WARNED'
       AND deadline_at IS NOT NULL AND deadline_at <= ?
       AND occupancy_state IN ('OCCUPIED', 'EXTENSION_PENDING')`,
  );

  const tx = db.transaction(() => {
    for (const row of rows) {
      if (row.warning_status === "WARNED" && row.deadline_at <= epoch) {
        markForgotten.run(epoch, row.locker_id, epoch);
      } else if (
        row.warning_status === "NONE" &&
        currentHour >= REMINDER_HOUR
      ) {
        updateWarning.run(
          today,
          jakartaMidnight(addCalendarDays(today, 2)),
          row.locker_id,
        );
      }
    }
  });
  tx();
}

function canAccessLocker(user, lockerId) {
  if (user.role === "admin") return true;

  const owner = getLockerOwner(lockerId);
  return owner?.id === user.id;
}

function getPendingCommand(lockerId) {
  return db
    .prepare(
      `SELECT command_id
       FROM commands
       WHERE locker_id = ? AND status = 'PENDING'
       ORDER BY created_at DESC
       LIMIT 1`,
    )
    .get(lockerId);
}

function validateUserAction(lockerId, action) {
  const owner = getLockerOwner(lockerId);
  if (!owner) return "Locker is not owned by a user";
  if (owner.warning_status === "FORGOTTEN") {
    return "Forgotten lockers must be resolved by an admin";
  }

  if (action === "LOCK" &&
      ["RESERVED", "DEPOSIT_OPEN"].includes(owner.occupancy_state)) return null;
  if (action === "UNLOCK" &&
      ["RESERVED", "OCCUPIED"].includes(owner.occupancy_state)) return null;

  return "Command is not valid for the current one-time-use cycle";
}

function applyAcknowledgedUserAction(command) {
  if (command.requested_by_role === "admin") return;

  const owner = getLockerOwner(command.locker_id);
  if (!owner || owner.id !== command.requested_by_user_id) return;

  if (command.action === "UNLOCK" && owner.occupancy_state === "RESERVED") {
    db.prepare(
      `UPDATE locker_ownership
       SET occupancy_state = 'DEPOSIT_OPEN'
       WHERE locker_id = ? AND user_id = ? AND occupancy_state = 'RESERVED'`,
    ).run(command.locker_id, command.requested_by_user_id);
    return;
  }

  if (command.action === "LOCK" &&
      ["RESERVED", "DEPOSIT_OPEN"].includes(owner.occupancy_state)) {
    db.prepare(
      `UPDATE locker_ownership
       SET unlock_count = 1, occupancy_state = 'OCCUPIED',
           reminder_date = NULL, deadline_at = NULL,
           warning_status = 'NONE', forgotten_at = NULL,
           checked_by = NULL, checked_at = NULL
       WHERE locker_id = ? AND user_id = ?
         AND occupancy_state IN ('RESERVED', 'DEPOSIT_OPEN')`,
    ).run(command.locker_id, command.requested_by_user_id);
    return;
  }

  if (command.action === "UNLOCK" && owner.occupancy_state === "OCCUPIED") {
    db.prepare(
      `UPDATE locker_ownership
       SET occupancy_state = 'EXTENSION_PENDING'
       WHERE locker_id = ? AND user_id = ? AND occupancy_state = 'OCCUPIED'`,
    ).run(command.locker_id, command.requested_by_user_id);
    publishLockerAssignment(command.locker_id, String(owner.id), false);
  }
}

function extractCommandIdFromPayload(payload) {
  if (!payload) return null;

  const parts = payload.split("|");

  if (parts.length >= 2) {
    if (parts[0].startsWith("CMD-")) return parts[0];
    if (
      ["ACCEPTED", "REJECTED"].includes(parts[0].toUpperCase()) &&
      parts[2]?.startsWith("CMD-")
    ) {
      return parts[2];
    }
    if (
      ["ACK", "NACK"].includes(parts[0].toUpperCase()) &&
      parts[1].startsWith("CMD-")
    ) {
      return parts[1];
    }
  }

  if (payload.startsWith("CMD-")) return payload.split("|")[0];

  return null;
}

// ============================================================
// MQTT
// ============================================================

function publishUserSync(fields) {
  if (!mqttConnected) return false;
  mqttClient.publish(
    `${TOPIC_PREFIX}/users/sync`,
    generateSignedUserSync(fields),
    { qos: 1, retain: false },
  );
  return true;
}

function publishUserCredentials(user) {
  if (!user?.device_pin_hash || !user?.device_pin_salt) return false;
  return publishUserSync([
    "UPSERT",
    String(user.id),
    user.device_rfid_uid ?? "",
    user.device_pin_salt,
    user.device_pin_hash,
  ]);
}

function publishUserDelete(userId) {
  return publishUserSync(["DELETE", String(userId)]);
}

function publishLockerAssignment(lockerId, userId, assign, open = true) {
  if (!mqttConnected) return false;
  if (assign) {
    const user = db.prepare(
      `SELECT id, device_rfid_uid, device_pin_salt, device_pin_hash
       FROM users WHERE id = ?`,
    ).get(Number(userId));
    if (!publishUserCredentials(user)) return false;
    return publishUserSync(["ASSIGN", lockerId, userId, open ? "OPEN" : "PRESERVE"]);
  }
  const unassigned = publishUserSync(["UNASSIGN", lockerId, userId]);
  return unassigned && publishUserDelete(userId);
}

function publishDeviceSnapshot() {
  if (!mqttConnected) return;
  publishUserSync(["UNASSIGN_ALL"]);
  publishUserSync(["CLEAR"]);

  const assignments = db.prepare(
    `SELECT o.locker_id, o.user_id, o.occupancy_state,
            u.device_rfid_uid, u.device_pin_salt, u.device_pin_hash
     FROM locker_ownership o
     JOIN users u ON u.id = o.user_id
     WHERE o.occupancy_state != 'EXTENSION_PENDING'
       AND u.device_pin_hash IS NOT NULL AND u.device_pin_salt IS NOT NULL
     ORDER BY o.locker_id`,
  ).all();
  for (const assignment of assignments) {
    publishUserCredentials(assignment);
    publishUserSync([
      "ASSIGN",
      assignment.locker_id,
      String(assignment.user_id),
      ["RESERVED", "DEPOSIT_OPEN"].includes(assignment.occupancy_state)
        ? "OPEN"
        : "PRESERVE",
    ]);
  }
}

function handleDeviceEvent(payload) {
  let event;
  try {
    event = JSON.parse(payload);
  } catch {
    console.log("Ignoring malformed device event.");
    return;
  }

  const lockerId = String(event.locker_id ?? "").toUpperCase();
  if (!hasLocker(lockerId)) return;
  if (typeof event.event_id === "string" && event.event_id) {
    const inserted = db.prepare(
      "INSERT OR IGNORE INTO device_events (event_id, received_at) VALUES (?, ?)",
    ).run(event.event_id, nowSeconds());
    if (inserted.changes === 0) return;
  }
  const locker = LOCKERS[lockerId];
  if (typeof event.locked === "boolean") {
    locker.state = event.locked ? "LOCKED" : "UNLOCKED";
  }
  locker.online = true;
  locker.last_ack = nowSeconds();

  const userId = Number(event.user_id);
  if (!Number.isSafeInteger(userId) || userId <= 0) return;
  const owner = getLockerOwner(lockerId);
  if (!owner || owner.id !== userId) return;

  switch (event.event) {
    case "LOCKER_ASSIGNED":
      db.prepare(
        `UPDATE locker_ownership
         SET occupancy_state = 'DEPOSIT_OPEN'
         WHERE locker_id = ? AND user_id = ? AND occupancy_state = 'RESERVED'`,
      ).run(lockerId, userId);
      break;
    case "DOOR_CLOSED_AUTO_LOCKED":
      db.prepare(
        `UPDATE locker_ownership
         SET unlock_count = 1, occupancy_state = 'OCCUPIED'
         WHERE locker_id = ? AND user_id = ?
           AND occupancy_state IN ('RESERVED', 'DEPOSIT_OPEN')`,
      ).run(lockerId, userId);
      break;
    case "LOCKER_UNLOCKED":
      db.prepare(
        `UPDATE locker_ownership
         SET unlock_count = 1, occupancy_state = 'EXTENSION_PENDING'
         WHERE locker_id = ? AND user_id = ?
           AND occupancy_state IN ('RESERVED', 'DEPOSIT_OPEN', 'OCCUPIED')`,
      ).run(lockerId, userId);
      publishLockerAssignment(lockerId, String(userId), false);
      break;
    case "LOCKER_EXPIRED":
      db.prepare(
        `UPDATE locker_ownership
         SET warning_status = 'FORGOTTEN', forgotten_at = ?, deadline_at = ?
         WHERE locker_id = ? AND user_id = ?`,
      ).run(nowSeconds(), nowSeconds(), lockerId, userId);
      break;
    case "LOCKER_RELEASED":
      db.prepare(
        "DELETE FROM locker_ownership WHERE locker_id = ? AND user_id = ?",
      ).run(lockerId, userId);
      publishUserDelete(String(userId));
      break;
    default:
      break;
  }
}

const RECONNECT_MIN_MS = 2000;
const RECONNECT_MAX_MS = 30000;

const mqttClient = mqtt.connect({
  protocol: MQTT_PROTOCOL,
  host: MQTT_HOST,
  port: MQTT_PORT,
  clientId: `smartlocker-backend-${crypto.randomBytes(4).toString("hex")}`,
  keepalive: 30,
  username: MQTT_USERNAME,
  password: MQTT_PASSWORD,
  rejectUnauthorized: MQTT_PROTOCOL === "mqtts",
  reconnectPeriod: RECONNECT_MIN_MS,
  // connect bersifat asynchronous: startup nggak nunggu broker.
});

mqttClient.on("connect", () => {
  mqttConnected = true;
  mqttClient.options.reconnectPeriod = RECONNECT_MIN_MS; // reset backoff
  console.log("Connected to MQTT broker.");

  mqttClient.subscribe(`${TOPIC_PREFIX}/+/status`, { qos: 0 });
  mqttClient.subscribe(`${TOPIC_PREFIX}/+/ack`, { qos: 0 });
  mqttClient.subscribe(`${TOPIC_PREFIX}/device/status`, { qos: 0 });
  mqttClient.subscribe(`${TOPIC_PREFIX}/device/event`, { qos: 1 });
  mqttClient.subscribe(`${TOPIC_PREFIX}/+/state`, { qos: 1 });
  mqttClient.subscribe(`${TOPIC_PREFIX}/users/request`, { qos: 1 });
  mqttClient.subscribe(`${TOPIC_PREFIX}/users/ack`, { qos: 1 });
});

mqttClient.on("close", () => {
  if (mqttConnected) console.log("MQTT disconnected.");
  mqttConnected = false;

  // Exponential backoff 2s -> 30s (setara reconnect_delay_set paho).
  mqttClient.options.reconnectPeriod = Math.min(
    mqttClient.options.reconnectPeriod * 2,
    RECONNECT_MAX_MS,
  );
});

mqttClient.on("error", (err) => {
  console.log(`MQTT error: ${err.message}`);
});

mqttClient.on("message", (topic, rawPayload) => {
  const payload = rawPayload.toString("utf8").trim();

  if (!topic) return;

  const parts = topic.replace(/^\/+|\/+$/g, "").split("/");
  if (parts.length !== 4) return;

  const [, , lockerId, messageType] = parts;

  if (lockerId === "users" && messageType === "request") {
    publishDeviceSnapshot();
    return;
  }
  if (lockerId === "users" && messageType === "ack") {
    console.log(`Device user sync acknowledgment: ${payload.split("|")[0]}`);
    return;
  }
  if (lockerId === "device" && messageType === "event") {
    handleDeviceEvent(payload);
    return;
  }

  // Device status (ONLINE/OFFLINE)
  if (lockerId === "device" && messageType === "status") {
    if (payload === "ONLINE" || payload === "OFFLINE") {
      DEVICE_ONLINE = payload === "ONLINE";
      console.log(`Device online status: ${DEVICE_ONLINE}`);
    }
    return;
  }

  // Firmware publishes JSON state; legacy builds publish a plain status string.
  if (messageType === "state") {
    if (!hasLocker(lockerId)) return;
    try {
      const state = JSON.parse(payload);
      if (typeof state.locked === "boolean") {
        LOCKERS[lockerId].state = state.locked ? "LOCKED" : "UNLOCKED";
      }
      LOCKERS[lockerId].online = true;
      LOCKERS[lockerId].last_ack = nowSeconds();
    } catch {
      console.log(`Ignoring malformed state from ${lockerId}.`);
    }
    return;
  }

  // Individual locker status
  if (messageType === "status") {
    if (!hasLocker(lockerId)) return;
    if (payload !== "LOCKED" && payload !== "UNLOCKED") return;

    LOCKERS[lockerId].state = payload;
    LOCKERS[lockerId].online = true;
    LOCKERS[lockerId].last_ack = nowSeconds();
    return;
  }

  // ACK from locker
  if (messageType === "ack") {
    const commandId = extractCommandIdFromPayload(payload);
    if (!commandId) return;
    const command = COMMANDS.get(commandId);
    if (!command || command.locker_id !== lockerId) return;

    const rejected = payload.split("|")[0].toUpperCase() === "REJECTED" ||
      payload.split("|")[0].toUpperCase() === "NACK";

    updateCommand(commandId, {
      status: rejected ? "FAILED" : "ACKNOWLEDGED",
      acknowledged_at: nowSeconds(),
      acknowledgment: payload,
      result: rejected ? "device_rejected" : "acknowledged",
    });
    if (!rejected) applyAcknowledgedUserAction(command);

    if (hasLocker(lockerId)) {
      LOCKERS[lockerId].online = true;
      LOCKERS[lockerId].last_ack = nowSeconds();
    }
  }
});

// ============================================================
// COMMAND TIMEOUT WATCHER
// ============================================================

let watchdogTimer = null;
let maintenanceTimer = null;

function commandTimeoutWatcher() {
  const now = nowSeconds();

  for (const command of COMMANDS.values()) {
    if (command.status !== "PENDING") continue;

    if (now - command.created_at > COMMAND_ACK_TIMEOUT) {
      command.status = "TIMED_OUT";
      command.result = "timeout";
      command.last_error = "Acknowledgment timeout";
      command.updated_at = now;

      persistCommand(command);
    }
  }
}

// ============================================================
// HTTP API
// ============================================================

const app = express();
app.disable("x-powered-by");
app.use(express.json());

app.use(cors({
  origin(origin, callback) {
    if (!origin || CORS_ORIGINS.includes(origin)) {
      return callback(null, true);
    }

    return callback(new Error("Origin is not allowed by CORS"));
  },
  methods: ["GET", "POST"],
  allowedHeaders: ["Content-Type", "Authorization"],
}));

const swaggerDocument = {
  openapi: "3.0.0",
  info: {
    title: "Smart Locker API",
    version: "1.0.0",
    description:
      "Authenticated API for monitoring and controlling Smart Locker devices.",
  },
  servers: [
    { url: "http://127.0.0.1:8000", description: "Local development" },
  ],
  components: {
    securitySchemes: {
      bearerAuth: {
        type: "http",
        scheme: "bearer",
        bearerFormat: "JWT",
      },
    },
  },
  paths: {
    "/auth/register/request-otp": {
      post: {
        summary: "Request an email verification code",
        requestBody: {
          required: true,
          content: {
            "application/json": {
              schema: {
                type: "object",
                required: ["username", "email", "password"],
                properties: {
                  username: { type: "string", example: "user1" },
                  email: { type: "string", format: "email", example: "user@example.com" },
                  password: { type: "string", format: "password", minLength: 8 },
                },
              },
            },
          },
        },
        responses: {
          202: { description: "Verification code requested" },
          503: { description: "Email delivery unavailable" },
        },
      },
    },
    "/auth/register/verify-otp": {
      post: {
        summary: "Verify email and activate a regular user",
        requestBody: {
          required: true,
          content: {
            "application/json": {
              schema: {
                type: "object",
                required: ["username", "email", "otp"],
                properties: {
                  username: { type: "string", example: "user1" },
                  email: { type: "string", format: "email" },
                  otp: { type: "string", minLength: 6, maxLength: 6 },
                },
              },
            },
          },
        },
        responses: {
          201: { description: "User activated" },
          400: { description: "Invalid or expired code" },
          429: { description: "Too many attempts" },
        },
      },
    },
    "/auth/login": {
      post: {
        summary: "Authenticate an operator",
        requestBody: {
          required: true,
          content: {
            "application/json": {
              schema: {
                type: "object",
                required: ["username", "password"],
                properties: {
                  username: { type: "string", example: "admin" },
                  password: { type: "string", format: "password" },
                },
              },
              "/me": {
                get: {
                  summary: "Get the authenticated user",
                  security: [{ bearerAuth: [] }],
                  responses: {
                    200: { description: "Authenticated user" },
                    401: { description: "Authentication required" },
                  },
                },
              },
            },
          },
        },
        responses: {
          200: { description: "JWT access token" },
          401: { description: "Invalid credentials" },
        },
      },
    },
    "/": {
      get: {
        summary: "Get backend health",
        responses: {
          200: { description: "Backend status" }
        }
      }
    },
    "/lockers": {
      get: {
        summary: "Get all locker and device statuses",
        security: [{ bearerAuth: [] }],
        responses: {
          200: { description: "Locker statuses" },
          401: { description: "Authentication required" },
        }
      }
    },
    "/lockers/{lockerId}/command": {
      post: {
        summary: "Send a lock or unlock command",
        security: [{ bearerAuth: [] }],
        parameters: [
          {
            name: "lockerId",
            in: "path",
            required: true,
            schema: { type: "string" }
          }
        ],
        requestBody: {
          required: true,
          content: {
            "application/json": {
              schema: {
                type: "object",
                required: ["action"],
                properties: {
                  action: {
                    type: "string",
                    enum: ["LOCK", "UNLOCK"]
                  }
                }
              }
            }
          }
        },
        responses: {
          200: { description: "Command created" },
          400: { description: "Invalid action" },
          401: { description: "Authentication required" },
          404: { description: "Locker not found" },
        }
      }
    },
    "/lockers/{lockerId}/status": {
      get: {
        summary: "Get one locker status",
        security: [{ bearerAuth: [] }],
        parameters: [
          {
            name: "lockerId",
            in: "path",
            required: true,
            schema: { type: "string" },
            example: "LOCKER_001"
          }
        ],
        responses: {
          200: { description: "Locker status" },
          401: { description: "Authentication required" },
          404: { description: "Locker not found" }
        }
      }
    },
    "/lockers/{lockerId}/claim": {
      post: {
        summary: "Claim an available locker",
        security: [{ bearerAuth: [] }],
        parameters: [
          {
            name: "lockerId",
            in: "path",
            required: true,
            schema: { type: "string" },
            example: "LOCKER_001",
          },
        ],
        responses: {
          201: { description: "Locker claimed" },
          401: { description: "Authentication required" },
          409: { description: "Locker occupied or user already owns a locker" },
        },
      },
    },
    "/lockers/{lockerId}/release": {
      post: {
        summary: "Release a locker owned by the user",
        security: [{ bearerAuth: [] }],
        parameters: [
          {
            name: "lockerId",
            in: "path",
            required: true,
            schema: { type: "string" },
            example: "LOCKER_001",
          },
        ],
        responses: {
          200: { description: "Locker released" },
          401: { description: "Authentication required" },
          403: { description: "Locker belongs to another user" },
        },
      },
    },
    "/commands/{commandId}": {
      get: {
        summary: "Get command delivery status",
        security: [{ bearerAuth: [] }],
        parameters: [
          {
            name: "commandId",
            in: "path",
            required: true,
            schema: { type: "string" },
            example: "CMD-1234567890ABCDEF"
          }
        ],
        responses: {
          200: { description: "Command status" },
          401: { description: "Authentication required" },
          404: { description: "Command not found" }
        }
      }
    }
  }
};

app.use("/docs", swaggerUi.serve, swaggerUi.setup(swaggerDocument));

const httpError = (res, status, detail) => res.status(status).json({ detail });

app.post("/auth/register/request-otp", async (req, res) => {
  const { username, email, password } = req.body ?? {};
  const normalizedEmail =
    typeof email === "string" ? normalizeEmail(email) : "";

  if (
    typeof username !== "string" ||
    typeof email !== "string" ||
    typeof password !== "string" ||
    !/^[A-Za-z0-9_]{3,32}$/.test(username) ||
    !isValidEmail(normalizedEmail) ||
    password.length < 8 ||
    password.length > 128
  ) {
    return httpError(
      res,
      422,
      "Username, email, and password are invalid",
    );
  }

  if (checkOtpRateLimit(normalizedEmail, req.ip)) {
    return httpError(res, 429, "Too many verification requests; try again later");
  }

  const activeConflict = db.prepare(
    "SELECT 1 FROM users WHERE username = ? OR email = ? LIMIT 1",
  ).get(username, normalizedEmail);
  if (activeConflict) {
    return httpError(res, 409, "Username or email is already registered");
  }

  const currentTime = nowSeconds();
  const existing = db.prepare(
    "SELECT last_sent_at FROM pending_registrations WHERE email = ? OR username = ? LIMIT 1",
  ).get(normalizedEmail, username);
  if (existing && Date.now() - existing.last_sent_at * 1000 < OTP_RESEND_COOLDOWN_MS) {
    return httpError(res, 429, "Please wait before requesting another code");
  }

  const otp = String(crypto.randomInt(0, 1_000_000)).padStart(6, "0");
  const pending = {
    username,
    email: normalizedEmail,
    passwordHash: hashPassword(password),
    otpHash: hashOtp(otp),
    expiresAt: currentTime + OTP_EXPIRY_SECONDS,
    lastSentAt: currentTime,
  };

  try {
    db.prepare(`
      INSERT INTO pending_registrations
        (username, email, password_hash, otp_hash, otp_expires_at, otp_attempts, last_sent_at, created_at, request_ip)
      VALUES (?, ?, ?, ?, ?, 0, ?, ?, ?)
      ON CONFLICT(email) DO UPDATE SET
        username = excluded.username, password_hash = excluded.password_hash,
        otp_hash = excluded.otp_hash, otp_expires_at = excluded.otp_expires_at,
        otp_attempts = 0, last_sent_at = excluded.last_sent_at,
        created_at = excluded.created_at, request_ip = excluded.request_ip
    `).run(
      pending.username, pending.email, pending.passwordHash, pending.otpHash,
      pending.expiresAt, pending.lastSentAt, currentTime, req.ip,
    );
  } catch (error) {
    if (error?.code === "SQLITE_CONSTRAINT_UNIQUE") {
      return httpError(res, 409, "Username or email is already pending registration");
    }
    throw error;
  }

  try {
    await sendRegistrationOtp(normalizedEmail, otp);
  } catch (error) {
    db.prepare("DELETE FROM pending_registrations WHERE email = ? AND otp_hash = ?")
      .run(normalizedEmail, pending.otpHash);
    if (error?.code === "EMAIL_NOT_CONFIGURED") {
      return httpError(res, 503, "Email verification is temporarily unavailable");
    }
    return httpError(res, 503, "Email verification could not be sent");
  }

  return res.status(202).json({
    message: "If the details can be registered, a verification code has been sent",
    expires_in: OTP_EXPIRY_SECONDS,
    resend_after: 60,
  });
});

app.post("/auth/register/verify-otp", (req, res) => {
  const { username, email, otp } = req.body ?? {};
  const normalizedEmail = typeof email === "string" ? normalizeEmail(email) : "";
  if (
    typeof username !== "string" ||
    typeof email !== "string" ||
    !isValidEmail(normalizedEmail) ||
    typeof otp !== "string" ||
    !/^\d{6}$/.test(otp)
  ) {
    return httpError(res, 422, "Username, email, and a 6-digit code are required");
  }

  const pending = db.prepare(
    "SELECT * FROM pending_registrations WHERE email = ? AND username = ?",
  ).get(normalizedEmail, username);
  if (!pending) return httpError(res, 400, "The verification code is invalid or expired");
  if (pending.otp_attempts >= OTP_MAX_ATTEMPTS) {
    return httpError(res, 429, "Too many verification attempts; request a new code");
  }
  if (pending.otp_expires_at < nowSeconds()) {
    return httpError(res, 400, "The verification code is invalid or expired");
  }

  if (!secureEqual(hashOtp(otp), pending.otp_hash)) {
    db.prepare(
      "UPDATE pending_registrations SET otp_attempts = otp_attempts + 1 WHERE id = ?",
    ).run(pending.id);
    return httpError(res, 400, "The verification code is invalid or expired");
  }

  try {
    const activate = db.transaction(() => {
      const current = db.prepare(
        "SELECT * FROM pending_registrations WHERE id = ?",
      ).get(pending.id);
      if (!current || current.otp_expires_at < nowSeconds() ||
          current.otp_attempts >= OTP_MAX_ATTEMPTS) {
        throw new Error("otp_invalid");
      }
      const result = db.prepare(
        `INSERT INTO users (username, email, password_hash, role, created_at)
         VALUES (?, ?, ?, 'user', ?)`,
      ).run(current.username, current.email, current.password_hash, nowSeconds());
      db.prepare("DELETE FROM pending_registrations WHERE id = ?").run(current.id);
      return result.lastInsertRowid;
    });
    const userId = activate();
    return res.status(201).json({
      user_id: userId,
      username,
      email: normalizedEmail,
      role: "user",
      message: "Registration verified. You can now sign in.",
    });
  } catch (error) {
    if (error?.message === "otp_invalid") {
      return httpError(res, 400, "The verification code is invalid or expired");
    }
    if (error?.code === "SQLITE_CONSTRAINT_UNIQUE") {
      return httpError(res, 409, "Username or email is already registered");
    }
    throw error;
  }
});

app.post("/auth/login", (req, res) => {
  const { username, password } = req.body ?? {};
  const clientKey = req.ip;
  const currentTime = Date.now();
  const attempt = loginAttempts.get(clientKey);

  if (attempt && currentTime - attempt.startedAt < LOGIN_WINDOW_MS) {
    if (attempt.failures >= MAX_LOGIN_FAILURES) {
      return httpError(res, 429, "Too many login attempts; try again later");
    }
  } else {
    loginAttempts.delete(clientKey);
  }

  if (typeof username !== "string" || typeof password !== "string") {
    return httpError(res, 422, "Username and password are required");
  }

  const user = db
    .prepare("SELECT id, username, password_hash, role FROM users WHERE username = ?")
    .get(username);
  const usernameMatches = Boolean(user);
  const passwordMatches = user ? verifyPassword(password, user.password_hash) : false;

  if (!usernameMatches || !passwordMatches) {
    const nextAttempt =
      attempt && currentTime - attempt.startedAt < LOGIN_WINDOW_MS
        ? attempt
        : { startedAt: currentTime, failures: 0 };
    nextAttempt.failures += 1;
    loginAttempts.set(clientKey, nextAttempt);
    return httpError(res, 401, "Invalid credentials");
  }

  loginAttempts.delete(clientKey);
  return res.json({
    access_token: createAccessToken(user),
    token_type: "Bearer",
    expires_in: 3600,
  });
});

app.get("/me", requireAuthentication, (req, res) => {
  const credentials = db.prepare(
    `SELECT device_rfid_uid, device_pin_hash
     FROM users WHERE id = ?`,
  ).get(req.auth.id);
  res.json({
    user_id: req.auth.id,
    username: req.auth.username,
    role: req.auth.role,
    device_credentials_configured: Boolean(credentials?.device_pin_hash),
    device_rfid_configured: Boolean(credentials?.device_rfid_uid),
  });
});

app.put("/me/device-credentials", requireAuthentication, (req, res) => {
  if (req.auth.role !== "user") {
    return httpError(res, 400, "Device credentials are only available for user accounts");
  }

  const pin = String(req.body?.pin ?? "");
  const rfidUid = String(req.body?.rfid_uid ?? "").trim().toUpperCase();
  if (!/^\d{4,8}$/.test(pin)) {
    return httpError(res, 422, "PIN must contain 4 to 8 digits");
  }
  if (rfidUid && !/^(?:[0-9A-F]{8}|[0-9A-F]{14}|[0-9A-F]{20})$/.test(rfidUid)) {
    return httpError(res, 422, "RFID UID must be 8, 14, or 20 hexadecimal characters");
  }

  if (rfidUid) {
    const duplicate = db.prepare(
      "SELECT id FROM users WHERE device_rfid_uid = ? AND id != ?",
    ).get(rfidUid, req.auth.id);
    if (duplicate) return httpError(res, 409, "RFID UID is already registered");
  }

  const salt = crypto.randomBytes(8).toString("hex");
  const pinHash = crypto.pbkdf2Sync(pin, salt, 10_000, 32, "sha256").toString("hex");
  db.prepare(
    `UPDATE users
     SET device_rfid_uid = ?, device_pin_salt = ?, device_pin_hash = ?
     WHERE id = ?`,
  ).run(rfidUid || null, salt, pinHash, req.auth.id);

  const user = db.prepare(
    `SELECT id, device_rfid_uid, device_pin_salt, device_pin_hash
     FROM users WHERE id = ?`,
  ).get(req.auth.id);
  const activeAssignment = db.prepare(
    "SELECT 1 FROM locker_ownership WHERE user_id = ?",
  ).get(req.auth.id);
  if (activeAssignment) publishUserCredentials(user);

  return res.json({
    device_credentials_configured: true,
    device_rfid_configured: Boolean(rfidUid),
  });
});

// ---- GET /
app.get("/", (_req, res) => {
  res.json({
    system: "Smart Locker Backend",
    status: "running",
    mqtt_connected: mqttConnected,
  });
});

// ---- GET /lockers
app.get("/lockers", requireAuthentication, (req, res) => {
  runLockerMaintenance();
  const lockers = Object.fromEntries(
    Object.keys(LOCKERS).map((id) => [id, getLockerView(id, req.auth)]),
  );

  const response = { lockers, server_now: nowSeconds(), time_zone: CAMPUS_TIME_ZONE };
  if (req.auth.role === "admin") {
    response.mqtt_connected = mqttConnected;
    response.device_online = DEVICE_ONLINE;
  }

  res.json(response);
});

// ---- GET /lockers/:lockerId/status
app.get("/lockers/:lockerId/status", requireAuthentication, (req, res) => {
  const lockerId = req.params.lockerId.toUpperCase();

  if (!hasLocker(lockerId)) return httpError(res, 404, "Locker not found");

  runLockerMaintenance();
  res.json(getLockerView(lockerId, req.auth));
});

app.post("/lockers/:lockerId/claim", requireAuthentication, (req, res) => {
  const lockerId = req.params.lockerId.toUpperCase();

  if (!hasLocker(lockerId)) return httpError(res, 404, "Locker not found");
  if (req.auth.role === "admin") {
    return httpError(res, 400, "Admin does not need to claim a locker");
  }
  const credentials = db.prepare(
    `SELECT device_pin_salt, device_pin_hash
     FROM users WHERE id = ?`,
  ).get(req.auth.id);
  if (!credentials?.device_pin_salt || !credentials?.device_pin_hash) {
    return httpError(
      res,
      409,
      "Set a device PIN in your profile before claiming a locker",
    );
  }
  const currentLocker = db
    .prepare("SELECT locker_id FROM locker_ownership WHERE user_id = ?")
    .get(req.auth.id);
  if (currentLocker) {
    return httpError(
      res,
      409,
      `You already own ${currentLocker.locker_id}; release it before claiming another locker`,
    );
  }

  try {
    db.prepare(
      `INSERT INTO locker_ownership
       (locker_id, user_id, assigned_at, unlock_count, occupancy_state)
       VALUES (?, ?, ?, 0, 'RESERVED')`,
    ).run(lockerId, req.auth.id, nowSeconds());
  } catch (error) {
    if (error?.code === "SQLITE_CONSTRAINT_PRIMARYKEY") {
      return httpError(res, 409, "Locker is already occupied");
    }
    if (error?.code === "SQLITE_CONSTRAINT_UNIQUE") {
      return httpError(res, 409, "You already own a locker");
    }
    if (error?.code === "SQLITE_CONSTRAINT_TRIGGER") {
      return httpError(res, 409, "You already own a locker");
    }
    throw error;
  }

  publishLockerAssignment(lockerId, String(req.auth.id), true);
  return res.status(201).json(getLockerView(lockerId, req.auth));
});

app.post("/lockers/:lockerId/release", requireAuthentication, (req, res) => {
  const lockerId = req.params.lockerId.toUpperCase();

  if (!hasLocker(lockerId)) return httpError(res, 404, "Locker not found");

  const owner = getLockerOwner(lockerId);
  if (!owner) return httpError(res, 409, "Locker is already available");
  if (req.auth.role !== "admin" && owner.id !== req.auth.id) {
    return httpError(res, 403, "You do not own this locker");
  }
  if (owner.warning_status === "FORGOTTEN") {
    return httpError(res, 409, "Forgotten lockers must be resolved by an admin");
  }

  db.prepare("DELETE FROM locker_ownership WHERE locker_id = ?").run(lockerId);
  publishLockerAssignment(lockerId, String(owner.id), false);
  return res.json(getLockerView(lockerId, req.auth));
});

app.post("/lockers/:lockerId/extend", requireAuthentication, (req, res) => {
  const lockerId = req.params.lockerId.toUpperCase();

  if (!hasLocker(lockerId)) return httpError(res, 404, "Locker not found");
  if (req.auth.role === "admin") {
    return httpError(res, 400, "Admin cannot extend a user cycle");
  }

  const owner = getLockerOwner(lockerId);
  if (!owner || owner.id !== req.auth.id) {
    return httpError(res, 403, "You do not own this locker");
  }
  if (owner.occupancy_state !== "EXTENSION_PENDING") {
    return httpError(res, 409, "Locker is not waiting for an extension decision");
  }
  if (owner.warning_status === "FORGOTTEN") {
    return httpError(res, 409, "Forgotten lockers must be resolved by an admin");
  }

  db.prepare(
    `UPDATE locker_ownership
     SET unlock_count = 1, occupancy_state = 'DEPOSIT_OPEN',
         reminder_date = NULL, deadline_at = NULL,
         warning_status = 'NONE', forgotten_at = NULL,
         checked_by = NULL, checked_at = NULL
     WHERE locker_id = ? AND user_id = ?
       AND occupancy_state = 'EXTENSION_PENDING'`,
  ).run(lockerId, req.auth.id);

  publishLockerAssignment(lockerId, String(req.auth.id), true);
  return res.json(getLockerView(lockerId, req.auth));
});

app.post("/lockers/:lockerId/finish", requireAuthentication, (req, res) => {
  const lockerId = req.params.lockerId.toUpperCase();

  if (!hasLocker(lockerId)) return httpError(res, 404, "Locker not found");
  if (req.auth.role === "admin") {
    return httpError(res, 400, "Admin cannot finish a user cycle");
  }

  const owner = getLockerOwner(lockerId);
  if (!owner || owner.id !== req.auth.id) {
    return httpError(res, 403, "You do not own this locker");
  }
  if (owner.occupancy_state !== "EXTENSION_PENDING") {
    return httpError(res, 409, "Locker is not ready to finish");
  }
  if (owner.warning_status === "FORGOTTEN") {
    return httpError(res, 409, "Forgotten lockers must be resolved by an admin");
  }

  db.prepare(
    `DELETE FROM locker_ownership
     WHERE locker_id = ? AND user_id = ?
       AND occupancy_state = 'EXTENSION_PENDING'`,
  ).run(lockerId, req.auth.id);

  publishLockerAssignment(lockerId, String(req.auth.id), false);
  return res.json(getLockerView(lockerId, req.auth));
});

app.get("/admin/forgotten-lockers", requireAuthentication, (req, res) => {
  if (req.auth.role !== "admin") return httpError(res, 403, "Admin access required");
  runLockerMaintenance();
  const rows = db.prepare(
    `SELECT locker_id, user_id, assigned_at, unlock_count, occupancy_state,
            reminder_date, deadline_at, warning_status, forgotten_at
     FROM locker_ownership
     WHERE warning_status = 'FORGOTTEN'
     ORDER BY forgotten_at ASC`,
  ).all();
  res.json({
    server_now: nowSeconds(),
    time_zone: CAMPUS_TIME_ZONE,
    lockers: rows.map((row) => ({
      ...getLockerView(row.locker_id, req.auth),
      owner: getLockerView(row.locker_id, req.auth).owner,
    })),
  });
});

app.post("/admin/forgotten-lockers/:lockerId/resolve", requireAuthentication, (req, res) => {
  if (req.auth.role !== "admin") return httpError(res, 403, "Admin access required");
  const lockerId = req.params.lockerId.toUpperCase();
  if (!hasLocker(lockerId)) return httpError(res, 404, "Locker not found");

  const resolve = db.transaction(() => {
    const owner = getLockerOwner(lockerId);
    if (!owner) throw new Error("already_available");
    if (owner.warning_status !== "FORGOTTEN") throw new Error("not_forgotten");
    const checkedAt = nowSeconds();
    db.prepare(
      `INSERT INTO forgotten_locker_resolutions
       (locker_id, user_id, checked_by, checked_at, released_at)
       VALUES (?, ?, ?, ?, ?)`,
    ).run(lockerId, owner.id, req.auth.id, checkedAt, checkedAt);
    db.prepare("DELETE FROM locker_ownership WHERE locker_id = ? AND warning_status = 'FORGOTTEN'")
      .run(lockerId);
    return { checkedAt, userId: owner.id };
  });

  try {
    const result = resolve();
    publishLockerAssignment(lockerId, String(result.userId), false);
    return res.json({
      message: "Forgotten locker checked and released",
      locker_id: lockerId,
      checked_by: req.auth.username,
      checked_at: result.checkedAt,
      locker: getLockerView(lockerId, req.auth),
    });
  } catch (error) {
    if (error.message === "already_available") return httpError(res, 409, "Locker is already available");
    if (error.message === "not_forgotten") return httpError(res, 409, "Locker is not forgotten");
    throw error;
  }
});

// ---- POST /lockers/:lockerId/command
app.post(
  "/lockers/:lockerId/command",
  requireAuthentication,
  (req, res) => {
  const lockerId = req.params.lockerId.toUpperCase();

  // Validasi body (setara pydantic -> 422).
  if (!req.body || typeof req.body.action !== "string") {
    return httpError(res, 422, "Field 'action' is required and must be a string");
  }
  const action = req.body.action.toUpperCase();

  if (!hasLocker(lockerId)) return httpError(res, 404, "Locker not found");
  if (!canAccessLocker(req.auth, lockerId)) {
    return httpError(res, 403, "You do not own this locker");
  }

  if (action !== "LOCK" && action !== "UNLOCK") {
    return httpError(res, 400, "Action must be LOCK or UNLOCK");
  }

  if (req.auth.role !== "admin") {
    const pending = getPendingCommand(lockerId);
    if (pending) {
      return httpError(res, 409, "A command for this locker is still pending");
    }

    const actionError = validateUserAction(lockerId, action);
    if (actionError) return httpError(res, 409, actionError);
  }

  // Generate signed command.
  const commandId = `CMD-${crypto.randomBytes(8).toString("hex").toUpperCase()}`;
  const payload = generateCommand(lockerId, action, commandId);
  const topic = lockerTopic(lockerId, "command");

  const currentTime = nowSeconds();

  // Determine delivery method.
  let delivery = "manual_serial";
  let status = "MANUAL_READY";
  let lastError = null;

  if (mqttConnected) {
    mqttClient.publish(topic, payload, { qos: 1, retain: false }, (err) => {
      // Callback dipanggil setelah PUBACK / gagal kirim.
      if (!err) return;
      console.log(`MQTT publish error for ${commandId}: ${err.message}`);

      const cmd = COMMANDS.get(commandId);
      if (cmd && cmd.status === "PENDING") {
        updateCommand(commandId, {
          status: "MANUAL_READY",
          result: "manual_serial",
          last_error: `MQTT publish error: ${err.message}`,
          },
        );
      }
    });

    delivery = "published_to_mqtt";
    status = "PENDING";
  }

  const command = {
    command_id: commandId,
    locker_id: lockerId,
    action,
    status,
    payload,
    topic,
    created_at: currentTime,
    updated_at: currentTime,
    retry_count: 0,
    last_attempt_at: delivery === "published_to_mqtt" ? currentTime : null,
    acknowledged_at: null,
    acknowledgment: null,
    result: delivery,
    last_error: lastError,
    requested_by_user_id: req.auth.id,
    requested_by_role: req.auth.role,
  };

  // Save command in memory and SQLite.
  COMMANDS.set(commandId, command);
  persistCommand(command);

  res.json({
    message:
      delivery === "published_to_mqtt"
        ? "Command published to MQTT"
        : "Command generated for manual Serial Monitor execution",
    command_id: commandId,
    locker_id: lockerId,
    action,
    status,
    topic,
    delivery,
    command_line: payload,
  });
});

// ---- GET /commands/:commandId
app.get("/commands/:commandId", requireAuthentication, (req, res) => {
  const command = COMMANDS.get(req.params.commandId);

  if (!command) return httpError(res, 404, "Command not found");
  if (
    req.auth.role !== "admin" &&
    command.requested_by_user_id !== req.auth.id
  ) {
    return httpError(res, 403, "You do not have access to this command");
  }

  const { payload: _omit, ...commandData } = command;
  res.json(commandData);
});

// ---- Error handler (JSON body rusak -> 422 ala FastAPI)
app.use((err, _req, res, _next) => {
  if (err?.type === "entity.parse.failed") {
    return httpError(res, 422, "Invalid JSON body");
  }
  if (err?.message === "Origin is not allowed by CORS") {
    return httpError(res, 403, "Origin is not allowed by CORS");
  }
  console.error(err);
  return httpError(res, 500, "Internal Server Error");
});

// ============================================================
// STARTUP & SHUTDOWN
// ============================================================

initializeDatabase();
loadCommandsFromDatabase();
runLockerMaintenance();

console.log("Starting Smart Locker backend...");

watchdogTimer = setInterval(commandTimeoutWatcher, 1000);
maintenanceTimer = setInterval(runLockerMaintenance, MAINTENANCE_INTERVAL_MS);

const server = app.listen(HTTP_PORT, HTTP_HOST, () => {
  console.log(`Smart Locker API listening on http://${HTTP_HOST}:${HTTP_PORT}`);
});

let shuttingDown = false;

function shutdown(signal) {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log(`Received ${signal}, shutting down...`);

  clearInterval(watchdogTimer);
  clearInterval(maintenanceTimer);

  mqttClient.end(false, () => {
    server.close(() => {
      db.close();
      console.log("Smart Locker backend stopped.");
      process.exit(0);
    });
    server.closeAllConnections?.();
  });

  // Safety net kalau ada yang nyangkut.
  setTimeout(() => process.exit(1), 5000).unref();
}

process.on("SIGINT", () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));
