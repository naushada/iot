import { Component, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../common/httpsvc.service';
import { PubSubService } from '../../common/pubsubsvc.service';
import { StatusSnapshot } from '../../common/app-globals';

@Component({
  selector: 'app-dashboard',
  templateUrl: './dashboard.component.html',
  styleUrls: ['./dashboard.component.scss']
})
export class DashboardComponent implements OnDestroy {
  status: StatusSnapshot | null = null;
  loading = true;
  private subs = new Subscription();

  constructor(private http: HttpsvcService, private pubsub: PubSubService) {}

  ngOnInit(): void {
    this.startLongPoll();
    // Also accept pubsub updates from main component
    this.subs.add(
      this.pubsub.on<StatusSnapshot>('status').subscribe(s => {
        this.status = s;
      })
    );
  }

  /// Long-poll loop: blocks until a state change or 30s timeout,
  /// then immediately re-subscribes.  Keeps the dashboard live
  /// within ~1 RTT of any state change.
  private startLongPoll(): void {
    const poll = (): void => {
      this.http.getStatusLongPoll(30).subscribe({
        next: (s) => {
          this.status = s;
          this.loading = false;
          poll();  // re-subscribe immediately
        },
        error: () => {
          // On error, fall back to a 10s poll and retry
          this.http.getStatus().subscribe({
            next: (s) => { this.status = s; this.loading = false; },
            error: () => { this.loading = false; }
          });
          setTimeout(() => poll(), 10000);
        }
      });
    };
    poll();
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
