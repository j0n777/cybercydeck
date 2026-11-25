// Feed do CyberCYDeck: junta maq_* (máquinas) e uso_cc_* (uso de IA) do cache num JSON só (docs/feed.md).
const rows = $input.all().map(i => i.json).filter(r => r && r.chave);
const agora = Date.now();
// troque pelos hostnames das suas máquinas: chave = host que o agente de monitoramento manda, valor = nome na tela
const NOMES = { servidor: 'Servidor', pi: 'Raspberry Pi' };
const ORDEM = ['servidor', 'pi'];   // ordem na tela; as outras vêm depois
const parse = (s) => { try { return JSON.parse(s); } catch (e) { return null; } };
const int = (v) => (typeof v === 'number' && isFinite(v)) ? Math.round(v) : -1;

const maquinas = [];
for (const r of rows.filter(r => r.chave.startsWith('maq_') && r.chave !== 'maq_teste')) {
  const d = parse(r.dados); if (!d) continue;
  const host = d.host || r.chave.slice(4);
  const idade = Math.round((agora - new Date(r.recebido_em).getTime()) / 1000);
  if (idade > 7 * 86400) continue;   // máquina aposentada: some da lista
  const discos = (d.discos || []).filter(x => x.total_gb >= 1 && !String(x.dispositivo || '').includes('zram'));
  const disco = discos.length ? Math.round(Math.max(...discos.map(x => x.pct || 0))) : -1;
  maquinas.push({
    id: host, n: NOMES[host] || host, on: idade < 600, idade_s: idade,
    cpu: int(d.cpu_pct), ram: int(d.ram_pct), temp: int(d.temp), disco, up: int(d.uptime_s),
    falhas: (d.servicos_falhos || []).length,
  });
}
maquinas.sort((a, b) => ((ORDEM.indexOf(a.id) + 1) || 99) - ((ORDEM.indexOf(b.id) + 1) || 99));

// uso de IA dos últimos 30 dias, só de máquinas que mandaram nos últimos 3 dias
const desde = new Date(agora - 30 * 86400e3).toISOString().slice(0, 10);
let custo = 0, tokens = 0; const porMaq = [];
for (const r of rows.filter(r => r.chave.startsWith('uso_cc_') && r.chave !== 'uso_cc_teste')) {
  if (agora - new Date(r.recebido_em).getTime() > 3 * 86400e3) continue;
  const d = parse(r.dados); const dias = Array.isArray(d) ? d : (d && d.daily) || [];
  let c = 0, t = 0;
  for (const x of dias) if (String(x.date || x.period || '') >= desde) { c += x.totalCost || 0; t += x.totalTokens || 0; }
  custo += c; tokens += t;
  const h = r.chave.slice(7); porMaq.push({ n: NOMES[h] || h, custo: Math.round(c * 100) / 100 });
}

let saude = 'ok';
if (maquinas.some(m => m.disco >= 85 || m.temp >= 80)) saude = 'alerta';
if (maquinas.some(m => !m.on || m.disco >= 95)) saude = 'critico';

return [{ json: {
  ok: true, saude, maquinas,
  ia: { custo_usd: Math.round(custo * 100) / 100, tokens, por_maquina: porMaq },
  gerado_em: new Date(agora).toISOString(),
} }];
