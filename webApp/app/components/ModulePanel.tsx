"use client";

import { useEffect, useRef, useState } from "react";
import { fmt } from "../../lib/format";
import type { DeviceHealth, ModuleDevice, ModulePoll } from "../../lib/types";
import { Icon, type IconName } from "./Icon";

const STATE_LABEL: Record<string, string> = {
  idle: "Pronto",
  calibrating: "Calibrando",
  in_mission: "Em missão",
  uploading: "Enviando dados",
};

const DEVICE_ICON: Record<string, IconName> = { imu: "orbit", gps: "satellite", mag: "compass", camera: "camera" };
const DEVICE_NAME: Record<string, string> = { imu: "IMU", gps: "GPS", mag: "MAG", camera_rgb: "Câmera RGB", camera_nir: "Câmera NIR" };

export function deviceHealth(device: ModuleDevice): DeviceHealth {
  if (!device.connected) return "error";
  return device.status ?? "ok";
}

const deviceIcon = (d: ModuleDevice): IconName => DEVICE_ICON[d.id] ?? DEVICE_ICON[d.id.split("_")[0]] ?? "sparkle";
const deviceName = (d: ModuleDevice) => DEVICE_NAME[d.id] ?? d.id.toUpperCase();
const HEALTH_LABEL: Record<DeviceHealth, string> = { ok: "OK", warning: "Atenção", error: "Falha" };
const HEALTH_ICON: Record<DeviceHealth, IconName> = { ok: "check", warning: "alert", error: "x" };

export function ModulePanel({ poll, lastSeen, now }: { poll: ModulePoll | null; lastSeen: number | null; now: number }) {
  const [open, setOpen] = useState(false);
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!open) return;
    const close = (event: MouseEvent) => !ref.current?.contains(event.target as Node) && setOpen(false);
    const key = (event: KeyboardEvent) => event.key === "Escape" && setOpen(false);
    window.addEventListener("mousedown", close);
    window.addEventListener("keydown", key);
    return () => { window.removeEventListener("mousedown", close); window.removeEventListener("keydown", key); };
  }, [open]);

  const connected = !!poll?.connected;
  const status = poll?.status; // sem conexão: último status conhecido
  const state = status?.state;
  const devices = status?.devices ?? [];
  const calibratedAt = status?.sensors?.calibrated_at ?? null;
  const seenAgo = lastSeen ? fmt.ago(new Date(lastSeen).toISOString(), now) : null;

  // Procurar o módulo nunca "falha": o polling continua até ele aparecer.
  const tone = !connected ? "searching" : state === "in_mission" ? "mission" : state === "idle" ? "online" : "busy";
  const worst = devices.reduce<DeviceHealth>((w, d) => {
    const h = deviceHealth(d);
    return h === "error" || w === "error" ? "error" : h === "warning" || w === "warning" ? "warning" : "ok";
  }, "ok");

  return (
    <div className="module-panel" ref={ref}>
      <button className={`module-pill tone-${tone}`} onClick={() => setOpen((v) => !v)} aria-expanded={open} aria-haspopup="dialog">
        {connected ? <span className="pulse-dot" /> : <span className="radar" aria-hidden="true" />}
        <span className="module-pill-text" role="status" aria-live="polite">
          <strong>{connected ? "Módulo conectado" : "Procurando o módulo…"}</strong>
          <span>
            {connected
              ? `${STATE_LABEL[state || ""] || state || "—"} · ${poll?.latencyMs ?? "—"} ms`
              : seenAgo ? `Visto pela última vez ${seenAgo}` : "Conecta assim que ele aparecer na rede"}
          </span>
        </span>
        {connected && devices.length > 0 && (
          <span className="device-dots" aria-label={`Dispositivos: ${HEALTH_LABEL[worst]}`}>
            {devices.slice(0, 5).map((d) => (
              <span key={d.id} className={`device-dot health-${deviceHealth(d)}`} title={`${deviceName(d)}: ${HEALTH_LABEL[deviceHealth(d)]}`}>
                {deviceName(d).replace("Câmera ", "")}
              </span>
            ))}
          </span>
        )}
        <Icon name="right" size={14} className={`pill-chevron ${open ? "open" : ""}`} />
      </button>

      {open && (
        <div className="module-popover" role="dialog" aria-label="Status do módulo">
          <header>
            <div>
              <strong>{status?.module_id ?? "Módulo"}</strong>
              <span>
                {status?.version ? `versão ${status.version}` : "versão —"}
                {status?.uptime_s != null ? ` · ligado há ${fmt.duration(status.uptime_s * 1000)}` : ""}
              </span>
            </div>
            <span className={`conn-badge ${connected ? "on" : ""}`}>
              {connected ? <><span className="pulse-dot" />{poll?.latencyMs ?? "—"} ms</> : <><span className="radar small" />procurando</>}
            </span>
          </header>

          {!connected && status && <p className="popover-note"><Icon name="clock" size={13} />Último status conhecido{seenAgo ? `, ${seenAgo}` : ""}.</p>}
          {!status && <p className="popover-note"><Icon name="wifiOff" size={13} />Nenhum status recebido ainda. O dashboard segue procurando o módulo a cada 2 s.</p>}

          {status && (
            <>
              <h4>Dispositivos</h4>
              {devices.length ? (
                <ul className="device-list">
                  {devices.map((d) => {
                    const h = deviceHealth(d);
                    const readings = Object.entries(d.data ?? {});
                    return (
                      <li key={d.id} className={`health-${h}`}>
                        <span className="device-icon"><Icon name={deviceIcon(d)} size={15} /></span>
                        <span className="device-main">
                          <strong>{d.label || deviceName(d)}</strong>
                          <span>
                            {!d.connected ? "Desconectado" : d.message || (readings.length ? readings.map(([k, v]) => `${k} ${v ?? "—"}`).join(" · ") : "Conectado")}
                          </span>
                        </span>
                        <span className={`health-badge health-${h}`}><Icon name={HEALTH_ICON[h]} size={11} stroke={2.4} />{HEALTH_LABEL[h]}</span>
                      </li>
                    );
                  })}
                </ul>
              ) : (
                <p className="popover-note">O módulo não informou dispositivos.</p>
              )}

              <h4>Calibração</h4>
              <div className={`calibration-row ${status.sensors?.calibrated ? "ok" : "warn"}`}>
                <Icon name={status.sensors?.calibrated ? "check" : "alert"} size={15} />
                <span>
                  <strong>{status.sensors?.calibrated ? "Sensores calibrados" : "Sem calibração válida"}</strong>
                  <span>{calibratedAt ? `Última: ${fmt.date(calibratedAt)} às ${fmt.time(calibratedAt, false)} · ${fmt.ago(calibratedAt, now)}` : "Nenhuma calibração registrada"}</span>
                </span>
              </div>

              {(status.upload?.pending ?? 0) > 0 && (
                <div className="calibration-row">
                  <Icon name="upload" size={15} />
                  <span><strong>{status.upload!.pending} captura(s) na fila</strong><span>Enviadas à estação ao fim da missão</span></span>
                </div>
              )}
            </>
          )}
        </div>
      )}
    </div>
  );
}
