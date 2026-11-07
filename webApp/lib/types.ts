// Tipos compartilhados entre servidor e cliente. Ver docs/CONTRATO.md.

export type Station = { latitude: number; longitude: number; name?: string | null };

export type Severity = "alta" | "media" | "baixa";

export type Hotspot = {
  id: number;
  north_m: number | null;
  east_m: number | null;
  severity: Severity;
  ndvi_drop: number;
  ndvi_mean: number;
  surrounding_ndvi_mean: number;
  area_m2: number | null;
  area_px: number;
  centroid_px: [number, number];
  bbox_px: [number, number, number, number];
  polygon: [number, number][];
};

export type HotspotLite = Pick<Hotspot, "id" | "north_m" | "east_m" | "severity" | "ndvi_drop" | "area_m2">;

export type EntryStatus = "processed" | "processing" | "error";

export type PositionSource = "gps+imu" | "imu";

export type EntrySummary = {
  id: string;
  missionId: string | null;
  sequence: number | null;
  timestamp: string | null;
  position: {
    north_m: number | null;
    east_m: number | null;
    height_m: number | null;
    source: PositionSource | null; // "imu" = dead reckoning, pouco confiável
    sigma_m: number | null;
  } | null;
  orientation: { roll_deg: number | null; pitch_deg: number | null; yaw_deg: number | null } | null;
  status: EntryStatus;
  error?: string;
  ndvi: { mean: number; median: number; vegetation_fraction: number } | null;
  hotspots: HotspotLite[];
  footprint: [number, number][] | null; // [north_m, east_m] TL, TR, BR, BL
  gsdCm: number | null;
  areaM2: number | null;
  crop: { width: number; height: number } | null;
  version: string; // muda quando a entrada é (re)processada
  survey: boolean; // entra no levantamento (mapa, focos, médias)
  flags: string[]; // motivos de ficar fora do levantamento
  alignmentUncertain: boolean; // módulo girando na captura
};

export type MissionRecord = {
  id: string;
  started_at: string | null;
  finished_at?: string | null;
  forced?: boolean;
  module_id?: string | null;
  synthetic?: boolean; // reconstruída a partir das capturas
  registered?: boolean; // o módulo enviou o registro (POST /mission)
  captures?: number | null; // fotos declaradas pelo módulo
  track?: TrackStats | null; // resumo do histórico de posições
};

export type TrackStats = { samples: number; distance_m: number; max_range_m: number; duration_s: number; imu_fraction: number };

export type DataPayload = {
  signature: string;
  station: Station | null;
  missions: MissionRecord[];
  entries: EntrySummary[];
  error?: string;
};

export type Analysis = {
  version: number;
  processed_at: string;
  image: { width: number; height: number };
  crop: { x1: number; y1: number; x2: number; y2: number };
  ndvi: null | {
    mean: number; std: number; p10: number; p25: number; median: number; p75: number; p90: number;
    vegetation_fraction: number; valid_fraction: number;
    histogram: { edges: number[]; counts: number[] };
  };
  detection: { vegetation_threshold: number | null; drop_threshold: number | null; candidates: number };
  hotspots: Hotspot[];
  geo: { georeferenced: boolean; gsd_cm: number | null; footprint_ne_m: [number, number][] | null; area_m2: number | null; camera_hfov_deg: number };
  warnings: string[];
};

// ===== Módulo =====

export type ModuleState = "idle" | "calibrating" | "in_mission" | "uploading";

export type CalibrationStep = "still" | "figure8" | "poses" | "done";

export type DeviceHealth = "ok" | "warning" | "error";

export type ModuleDevice = {
  id: string; // "imu", "gps", "mag", "camera_rgb"…
  label?: string | null; // nome para exibir (ex.: "IMU · BNO085")
  connected: boolean;
  status?: DeviceHealth | null; // omitido: "ok" se conectado, "error" se não
  message?: string | null; // ex.: "sem fix", "taxa baixa"
  data?: Record<string, string | number | boolean | null> | null; // leituras curtas
};

export type ModuleStatus = {
  module_id?: string;
  version?: string | null;
  uptime_s?: number | null;
  state: ModuleState;
  devices?: ModuleDevice[] | null;
  sensors?: { calibrated?: boolean; calibrated_at?: string | null } | null;
  calibration?: {
    started_at?: string;
    step?: CalibrationStep | null;
    instruction?: string | null;
    remaining_s?: number | null;
    progress?: number | null;
  } | null;
  mission?: {
    id: string;
    started_at: string;
    captures_enabled_at?: string | null;
    captures?: number;
    last_capture_at?: string | null;
    ready_to_finish?: boolean;
  } | null;
  upload?: { pending?: number; sent?: number; total?: number } | null;
  live?: { north_m?: number | null; east_m?: number | null; height_m?: number | null; yaw_deg?: number | null } | null;
};

export type ModulePoll = {
  connected: boolean;
  latencyMs: number | null;
  checkedAt: string;
  status: ModuleStatus | null;
  error?: string;
};
