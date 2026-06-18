import { Component, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { DataStoreService } from '../../common/datastore.service';
import { PubSubService } from '../../common/pubsubsvc.service';
import { StatusSnapshot, fmtDuration } from '../../common/app-globals';

@Component({
  selector: 'app-dashboard',
  templateUrl: './dashboard.component.html',
  styleUrls: ['./dashboard.component.scss']
})
export class DashboardComponent implements OnDestroy {
  status: StatusSnapshot | null = null;
  loading = true;
  private subs = new Subscription();

  constructor(private ds: DataStoreService, private pubsub: PubSubService) {}

  /// Read the full status live off the single shared /status stream — no
  /// per-page long-poll. The BehaviorSubject replays the latest snapshot,
  /// so the dashboard paints instantly.
  ngOnInit(): void {
    this.subs.add(
      this.ds.observeStatus().subscribe(s => { this.status = s; this.loading = false; })
    );
    // Also accept pubsub updates from main component (one-shot login seed).
    this.subs.add(
      this.pubsub.on<StatusSnapshot>('status').subscribe(s => { this.status = s; })
    );
  }

  ngOnDestroy(): void { this.subs.unsubscribe(); }

  // ── Helpers ─────────────────────────────────────────────────────

  cardClass(state: string | undefined, upStates: string[]): string {
    if (!state) return 'disconnected';
    return upStates.includes(state.toLowerCase()) ? 'connected' : 'disconnected';
  }

  wifiClass(): string {
    return this.cardClass(this.status?.wifi?.state, ['connected']);
  }
  wanClass(): string {
    const a = this.status?.wan?.active_iface;
    return a && a.length > 0 ? 'connected' : 'disconnected';
  }

  // ── VPN connection (reuses vpn.state; professional 3-state label) ──
  vpnLabel(): string {
    const s = (this.status?.vpn?.state || '').toLowerCase();
    if (s === 'connected') return 'Connected';
    if (!s || s === 'disconnected' || s === 'exited') return 'Disconnected';
    return 'Connecting';   // resolving / auth / wait / connecting
  }
  vpnClass(): string {
    const s = (this.status?.vpn?.state || '').toLowerCase();
    if (s === 'connected') return 'connected';
    if (!s || s === 'disconnected' || s === 'exited') return 'disconnected';
    return 'starting';
  }
  /// Tunnel uptime = now − vpn.connected.unix, humanised. Empty when the tunnel
  /// is not connected or the connect timestamp isn't published yet (older
  /// firmware), so the row simply hides via *ngIf.
  vpnUptime(): string {
    const t = this.status?.vpn?.connected_unix;
    if (!t || (this.status?.vpn?.state || '').toLowerCase() !== 'connected') return '';
    return fmtDuration(Math.floor(Date.now() / 1000) - t);
  }

  // ── LwM2M connection lifecycle (iot.conn.state) ───────────────────
  private readonly lwm2mLabels: Record<string, string> = {
    'idle':          'Disconnected',
    'bootstrapping': 'Bootstrap: Connecting',
    'bootstrapped':  'Bootstrap: Connected',
    'dm-connecting': 'Device Management: Connecting',
    'dm-connected':  'Device Management: Connected',
    'registered':    'LwM2M Connected',
    'failed':        'Connection Failed',
  };
  lwm2mLabel(): string {
    return this.lwm2mLabels[this.status?.lwm2m?.conn_state || 'idle']
        || 'Disconnected';
  }
  lwm2mClass(): string {
    const s = this.status?.lwm2m?.conn_state;
    if (s === 'registered') return 'connected';
    if (!s || s === 'idle' || s === 'failed') return 'disconnected';
    return 'starting';   // bootstrapping / bootstrapped / dm-* in progress
  }
}
