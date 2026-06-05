import { Component, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../common/httpsvc.service';
import { PubSubService } from '../../common/pubsubsvc.service';
import { StatusSnapshot, ServicesStatus, VpnStatus, WifiStatus, WanStatus } from '../../common/app-globals';

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

  vpnClass(): string {
    return this.cardClass(this.status?.vpn?.state, ['connected']);
  }
  wifiClass(): string {
    return this.cardClass(this.status?.wifi?.state, ['connected']);
  }
  wanClass(): string {
    const a = this.status?.wan?.active_iface;
    return a && a.length > 0 ? 'connected' : 'disconnected';
  }

  lwm2mState(): string {
    return this.status?.lwm2m?.server_uri ? 'configured' : 'unconfigured';
  }

  serviceKeys(): string[] {
    if (!this.status?.services) return [];
    return Object.keys(this.status.services).sort();
  }

  svcLabel(key: string): string {
    return key.replace(/_/g, '.');
  }

  svcInfo(key: string) {
    const svcs = this.status?.services as Record<string, { enable?: boolean; state?: string }> | undefined;
    return svcs?.[key] || {};
  }
}
