const tz = process.env.NEXT_PUBLIC_TIMEZONE || "America/Sao_Paulo";

export const fmt = {
  number(value: number | null | undefined, digits = 1) {
    if (value === null || value === undefined || !Number.isFinite(value)) return "—";
    return value.toLocaleString("pt-BR", { minimumFractionDigits: digits, maximumFractionDigits: digits });
  },
  signed(value: number | null | undefined, digits = 2) {
    if (value === null || value === undefined || !Number.isFinite(value)) return "—";
    return `${value > 0 ? "+" : value < 0 ? "−" : ""}${Math.abs(value).toLocaleString("pt-BR", { minimumFractionDigits: digits, maximumFractionDigits: digits })}`;
  },
  percent(value: number | null | undefined, digits = 0) {
    if (value === null || value === undefined || !Number.isFinite(value)) return "—";
    return `${(value * 100).toLocaleString("pt-BR", { maximumFractionDigits: digits })}%`;
  },
  area(m2: number | null | undefined) {
    if (m2 === null || m2 === undefined || !Number.isFinite(m2)) return "—";
    if (m2 >= 10_000) return `${(m2 / 10_000).toLocaleString("pt-BR", { maximumFractionDigits: 2 })} ha`;
    return `${Math.round(m2).toLocaleString("pt-BR")} m²`;
  },
  distance(m: number | null | undefined) {
    if (m === null || m === undefined || !Number.isFinite(m)) return "—";
    return m >= 1000 ? `${(m / 1000).toLocaleString("pt-BR", { maximumFractionDigits: 2 })} km` : `${Math.round(m).toLocaleString("pt-BR")} m`;
  },
  date(value: string | null | undefined, year = true) {
    if (!value) return "—";
    const d = new Date(value);
    if (!Number.isFinite(d.getTime())) return "—";
    const parts = Object.fromEntries(
      new Intl.DateTimeFormat("pt-BR", { day: "2-digit", month: "short", year: "numeric", timeZone: tz }).formatToParts(d).map((p) => [p.type, p.value]),
    );
    return `${parts.day} ${String(parts.month).replace(".", "")}${year ? ` ${parts.year}` : ""}`;
  },
  time(value: string | null | undefined, seconds = true) {
    if (!value) return "—";
    const d = new Date(value);
    return Number.isFinite(d.getTime()) ? d.toLocaleTimeString("pt-BR", { hour: "2-digit", minute: "2-digit", second: seconds ? "2-digit" : undefined, hour12: false, timeZone: tz }) : "—";
  },
  duration(ms: number | null | undefined) {
    if (ms === null || ms === undefined || !Number.isFinite(ms) || ms < 0) return "—";
    const s = Math.floor(ms / 1000);
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
    return h ? `${h}h${String(m).padStart(2, "0")}` : `${String(m).padStart(2, "0")}:${String(r).padStart(2, "0")}`;
  },
  ago(value: string | null | undefined, now = Date.now()) {
    if (!value) return "—";
    const s = Math.max(0, Math.round((now - Date.parse(value)) / 1000));
    if (s < 60) return `há ${s} s`;
    if (s < 3600) return `há ${Math.floor(s / 60)} min`;
    return `há ${Math.floor(s / 3600)} h`;
  },
};
