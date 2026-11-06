"use client";

import { useEffect, useRef, useState } from "react";
import type { ModulePoll, Station } from "../../lib/types";
import { Icon, type IconName } from "./Icon";

function Modal({ children, onClose, labelledBy }: { children: React.ReactNode; onClose: () => void; labelledBy: string }) {
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const key = (event: KeyboardEvent) => event.key === "Escape" && onClose();
    window.addEventListener("keydown", key);
    ref.current?.querySelector<HTMLElement>("[data-autofocus]")?.focus();
    return () => window.removeEventListener("keydown", key);
  }, [onClose]);
  return (
    <div className="modal-backdrop" onMouseDown={(event) => event.target === event.currentTarget && onClose()}>
      <div className="modal" role="dialog" aria-modal="true" aria-labelledby={labelledBy} ref={ref}>
        {children}
      </div>
    </div>
  );
}

export function ConfirmDialog({
  icon,
  tone = "primary",
  title,
  children,
  confirm,
  onConfirm,
  onClose,
}: {
  icon: IconName;
  tone?: "primary" | "danger";
  title: string;
  children: React.ReactNode;
  confirm: string;
  onConfirm: () => void;
  onClose: () => void;
}) {
  return (
    <Modal onClose={onClose} labelledBy="confirm-title">
      <div className={`modal-icon tone-${tone}`}><Icon name={icon} size={22} /></div>
      <h2 id="confirm-title">{title}</h2>
      <div className="modal-text">{children}</div>
      <div className="modal-actions">
        <button className="btn btn-ghost" onClick={onClose}>Cancelar</button>
        <button className={`btn ${tone === "danger" ? "btn-danger" : "btn-primary"}`} onClick={onConfirm} data-autofocus>{confirm}</button>
      </div>
    </Modal>
  );
}

// ===== Calibração =====
// O módulo calibra em etapas (status.calibration.step); cada uma pede um
// movimento diferente. O texto vem do módulo (instruction); a ilustração e
// as dicas são do dashboard.

const FIGURE_EIGHT = "M60 40 C 60 18, 100 18, 100 40 C 100 62, 60 62, 60 40 C 60 18, 20 18, 20 40 C 20 62, 60 62, 60 40 Z";

type Step = "still" | "figure8" | "poses";
const STEPS: { id: Step; label: string; sensor: string; title: string; hint: string }[] = [
  { id: "still", label: "Parado", sensor: "Giroscópio", title: "Deixe o módulo parado", hint: "Apoie numa superfície firme e não toque até a etapa terminar." },
  { id: "figure8", label: "Oito", sensor: "Magnetômetro", title: "Gire o módulo fazendo um oito", hint: "Longe de metais e eletrônicos, desenhe um oito no ar devagar, girando o pulso." },
  { id: "poses", label: "Faces", sensor: "Acelerômetro", title: "Apoie o módulo em cada face", hint: "Deixe-o parado alguns segundos em cada lado, como um dado." },
];

function StepArt({ step }: { step: Step }) {
  if (step === "still") {
    return (
      <svg viewBox="0 0 120 80" className="art-still">
        <line x1="22" x2="98" y1="58" y2="58" className="art-ground" />
        <circle cx="60" cy="43" r="24" className="art-hold" />
        <g transform="translate(60 49)"><rect x="-14" y="-9" width="28" height="18" rx="4" className="art-device" /><circle r="2.4" cx="6" className="art-lens" /></g>
      </svg>
    );
  }
  if (step === "poses") {
    return (
      <svg viewBox="0 0 120 80" className="art-poses">
        <line x1="22" x2="98" y1="62" y2="62" className="art-ground" />
        <g className="art-tumble">
          <rect x="-13" y="-13" width="26" height="26" rx="4" className="art-device" />
          <circle r="2.6" cx="6" cy="-6" className="art-lens" />
          <path d="M-6 6h6" className="art-mark" />
        </g>
      </svg>
    );
  }
  return (
    <svg viewBox="0 0 120 80">
      <path d={FIGURE_EIGHT} className="eight-track" />
      <path d={FIGURE_EIGHT} className="eight-trail" pathLength={100} />
      <g className="eight-device">
        <rect x="-7" y="-5" width="14" height="10" rx="2.5" />
        <circle r="1.6" cx="3" />
      </g>
    </svg>
  );
}

export function CalibrationDialog({ poll, onClose }: { poll: ModulePoll | null; onClose: () => void }) {
  const [phase, setPhase] = useState<"running" | "done">("running");
  const seen = useRef(false);
  const state = poll?.status?.state;
  const calibration = poll?.status?.calibration;

  useEffect(() => {
    if (state === "calibrating") seen.current = true;
    else if (seen.current && poll?.connected && state === "idle") setPhase("done");
  }, [state, poll?.connected]);

  const reported = calibration?.step;
  const finished = phase === "done" || reported === "done";
  const current = STEPS.find((s) => s.id === reported) ?? null;
  const index = current ? STEPS.indexOf(current) : -1;
  const progress = calibration?.progress ?? null;
  const remaining = calibration?.remaining_s ?? null;
  const waiting = state !== "calibrating" && !seen.current;

  if (finished && phase === "done") {
    return (
      <Modal onClose={onClose} labelledBy="calibration-title">
        <div className="modal-icon tone-success"><Icon name="check" size={24} stroke={2.2} /></div>
        <h2 id="calibration-title">Sensores calibrados</h2>
        <p className="modal-text">Giroscópio, magnetômetro e acelerômetro calibrados. O módulo está pronto para iniciar uma missão.</p>
        <div className="modal-actions"><button className="btn btn-primary" onClick={onClose} data-autofocus>Concluir</button></div>
      </Modal>
    );
  }

  return (
    <Modal onClose={onClose} labelledBy="calibration-title">
      <ol className="cal-steps" aria-label="Etapas da calibração">
        {STEPS.map((s, i) => {
          const status = reported === "done" || (index >= 0 && i < index) ? "done" : i === index ? "active" : "todo";
          return (
            <li key={s.id} className={`cal-step ${status}`} aria-current={status === "active" ? "step" : undefined}>
              <span className="cal-step-dot">{status === "done" ? <Icon name="check" size={12} stroke={2.6} /> : i + 1}</span>
              <span><strong>{s.label}</strong><small>{s.sensor}</small></span>
            </li>
          );
        })}
      </ol>

      <div className="figure-eight" aria-hidden="true" key={current?.id ?? "none"}>
        <StepArt step={current?.id ?? "figure8"} />
      </div>

      <h2 id="calibration-title" aria-live="polite">
        {waiting ? "Iniciando calibração…" : calibration?.instruction || current?.title || "Siga as instruções do módulo"}
      </h2>
      {current && <p className="modal-text">{current.hint}</p>}

      <div className="calibration-progress">
        <div className="banner-progress">
          <i className={progress === null ? "indeterminate" : ""} style={progress !== null ? { width: `${progress * 100}%` } : undefined} />
        </div>
        <span className="mono">
          {!poll?.connected ? "Conexão perdida…" : waiting ? "Aguardando o módulo…" : remaining !== null ? `${Math.ceil(remaining)} s restantes` : progress !== null ? `${Math.round(progress * 100)}%` : "Calibrando…"}
        </span>
      </div>
      <div className="modal-actions"><button className="btn btn-ghost" onClick={onClose}>Ocultar</button></div>
    </Modal>
  );
}

// ===== Posição da estação =====

export function StationPanel({
  station,
  placing,
  pending,
  onStartPlacing,
  onCancel,
  onSave,
  onRemove,
}: {
  station: Station | null;
  placing: boolean;
  pending: [number, number] | null;
  onStartPlacing: () => void;
  onCancel: () => void;
  onSave: (lngLat: [number, number]) => void;
  onRemove: () => void;
}) {
  const [lat, setLat] = useState("");
  const [lon, setLon] = useState("");
  useEffect(() => {
    const source = pending ? { latitude: pending[1], longitude: pending[0] } : station;
    if (source) {
      setLat(source.latitude.toFixed(7));
      setLon(source.longitude.toFixed(7));
    }
  }, [pending, station]);

  if (!placing) {
    if (station) return null;
    return (
      <div className="station-callout">
        <div className="modal-icon tone-primary small"><Icon name="home" size={18} /></div>
        <div>
          <strong>Onde fica a estação?</strong>
          <span>As posições do módulo são relativas ao ponto de partida. Marque a estação no mapa para posicionar as fotos.</span>
        </div>
        <button className="btn btn-primary" onClick={onStartPlacing}><Icon name="crosshair" size={15} />Marcar no mapa</button>
      </div>
    );
  }

  const valid = Number.isFinite(parseFloat(lat)) && Number.isFinite(parseFloat(lon));
  return (
    <div className="station-callout placing">
      <div className="modal-icon tone-primary small"><Icon name="crosshair" size={18} /></div>
      <div>
        <strong>Clique no mapa onde a estação está</strong>
        <span>Ou digite as coordenadas.</span>
        <div className="coord-inputs">
          <label>Lat<input value={lat} onChange={(e) => setLat(e.target.value)} inputMode="decimal" /></label>
          <label>Lon<input value={lon} onChange={(e) => setLon(e.target.value)} inputMode="decimal" /></label>
        </div>
      </div>
      <div className="callout-actions">
        {station && (
          <button className="btn btn-ghost btn-remove" onClick={onRemove} title="Apagar a posição salva da estação">
            <Icon name="x" size={14} />Remover
          </button>
        )}
        <button className="btn btn-ghost" onClick={onCancel}>Cancelar</button>
        <button className="btn btn-primary" disabled={!valid} onClick={() => onSave([parseFloat(lon), parseFloat(lat)])}>Salvar</button>
      </div>
    </div>
  );
}

// ===== Avisos =====

export type Toast = { id: number; tone: "error" | "success" | "info"; title: string; text?: string };

export function Toasts({ toasts, onDismiss }: { toasts: Toast[]; onDismiss: (id: number) => void }) {
  return (
    <div className="toasts" aria-live="polite">
      {toasts.map((t) => (
        <div key={t.id} className={`toast toast-${t.tone}`} role={t.tone === "error" ? "alert" : "status"}>
          <Icon name={t.tone === "error" ? "alert" : t.tone === "success" ? "check" : "sparkle"} size={16} />
          <div><strong>{t.title}</strong>{t.text && <span>{t.text}</span>}</div>
          <button onClick={() => onDismiss(t.id)} aria-label="Dispensar"><Icon name="x" size={14} /></button>
        </div>
      ))}
    </div>
  );
}
