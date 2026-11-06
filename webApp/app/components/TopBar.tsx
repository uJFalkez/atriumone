"use client";

import Image from "next/image";
import { useEffect, useRef, useState } from "react";
import logo from "../../assets/logo_white.png";
import { fmt } from "../../lib/format";
import type { ModulePoll } from "../../lib/types";
import { Icon } from "./Icon";
import { ModulePanel, deviceHealth } from "./ModulePanel";

export type Command = "mission/start" | "mission/finish" | "mission/finish?force" | "calibrate";

export function TopBar({
  poll,
  lastSeen,
  now,
  busy,
  onCommand,
}: {
  poll: ModulePoll | null;
  lastSeen: number | null;
  now: number;
  busy: Command | null;
  onCommand: (command: Command) => void;
}) {
  const [menu, setMenu] = useState(false);
  const menuRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!menu) return;
    const close = (event: MouseEvent) => !menuRef.current?.contains(event.target as Node) && setMenu(false);
    window.addEventListener("mousedown", close);
    return () => window.removeEventListener("mousedown", close);
  }, [menu]);

  const connected = !!poll?.connected;
  const status = poll?.status;
  const devices = status?.devices ?? [];
  const state = status?.state;
  const mission = status?.mission;
  const ready = !!mission?.ready_to_finish;
  const inMission = state === "in_mission";
  const uploading = state === "uploading";

  // Regras de habilitação:
  // - nada funciona sem conexão;
  // - calibrar só com o módulo parado (fora de missão);
  // - "Iniciar" vira "Finalizar" durante a missão, liberado quando o módulo
  //   reporta que voltou à estação.
  const canCalibrate = connected && state === "idle" && !busy;
  const canStart = connected && state === "idle" && !busy;
  const canFinish = connected && inMission && ready && !busy;

  const calibrateHint = !connected
    ? "Módulo desconectado"
    : state === "idle"
      ? "Calibrar IMU e magnetômetro"
      : inMission || uploading
        ? "Indisponível durante a missão"
        : "Calibração em andamento";

  let primary: React.ReactNode;
  if (inMission) {
    primary = (
      <div className="split-button" ref={menuRef}>
        <button
          className={`btn btn-finish ${ready ? "is-ready" : ""}`}
          disabled={!canFinish}
          onClick={() => onCommand("mission/finish")}
          title={ready ? "O módulo voltou à estação" : "Liberado quando o módulo voltar à estação"}
        >
          {busy === "mission/finish" ? <span className="spinner" /> : <Icon name="flag" size={16} />}
          Finalizar missão
        </button>
        <button className="btn btn-finish btn-more" disabled={!connected || !!busy} onClick={() => setMenu((v) => !v)} aria-label="Mais opções" aria-expanded={menu}>
          <Icon name="more" size={16} stroke={3} />
        </button>
        {menu && (
          <div className="menu" role="menu">
            <button role="menuitem" onClick={() => { setMenu(false); onCommand("mission/finish?force"); }}>
              <Icon name="alert" size={15} />
              <span><strong>Forçar finalização</strong><small>Encerra sem aguardar o retorno</small></span>
            </button>
          </div>
        )}
      </div>
    );
  } else if (uploading) {
    const upload = status?.upload;
    primary = (
      <button className="btn btn-primary" disabled>
        <Icon name="upload" size={16} />
        Enviando {upload?.sent ?? 0}/{upload?.total ?? (upload?.pending ?? 0) + (upload?.sent ?? 0)}
      </button>
    );
  } else {
    primary = (
      <button className="btn btn-primary" disabled={!canStart} onClick={() => onCommand("mission/start")} title={connected ? undefined : "Módulo desconectado"}>
        {busy === "mission/start" ? <span className="spinner" /> : <Icon name="play" size={14} />}
        Iniciar missão
      </button>
    );
  }

  return (
    <header className="topbar">
      <div className="brand">
        <Image src={logo} alt="" width={26} height={30} priority />
        <div>
          <strong>Atrium One</strong>
          <span>Estação de solo</span>
        </div>
      </div>

      <div className="topbar-center">
        <ModulePanel poll={poll} lastSeen={lastSeen} now={now} />
        {connected && status?.sensors && (
          <div className={`chip ${status.sensors.calibrated ? "" : "chip-warn"}`}>
            <Icon name={status.sensors.calibrated ? "check" : "alert"} size={14} />
            {status.sensors.calibrated
              ? status.sensors.calibrated_at ? `Calibrado ${fmt.ago(status.sensors.calibrated_at, now)}` : "Sensores calibrados"
              : "Sensores sem calibração"}
          </div>
        )}
        {connected && devices.some((d) => deviceHealth(d) === "error") && (
          <div className="chip chip-error"><Icon name="alert" size={14} />{devices.filter((d) => deviceHealth(d) === "error").length} dispositivo(s) com falha</div>
        )}
        {connected && (status?.upload?.pending ?? 0) > 0 && !uploading && (
          <div className="chip"><Icon name="upload" size={14} /> {status!.upload!.pending} na fila</div>
        )}
      </div>

      <div className="topbar-actions">
        <button className="btn btn-ghost" disabled={!canCalibrate} onClick={() => onCommand("calibrate")} title={calibrateHint}>
          {busy === "calibrate" ? <span className="spinner" /> : <Icon name="orbit" size={16} />}
          Calibrar sensores
        </button>
        {primary}
      </div>
    </header>
  );
}
