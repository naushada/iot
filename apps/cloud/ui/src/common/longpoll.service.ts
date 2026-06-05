import { Injectable, OnDestroy } from '@angular/core';
import { Subscription, Subject, timer } from 'rxjs';
import { HttpsvcService } from './httpsvc.service';

/// Long-poll watcher for real-time data-store updates.
///
/// Opens a GET /api/v1/db/get?key=X&timeout=30 long-poll for each key.
/// The server holds the connection until the value changes (or 30s
/// elapses), then returns.  We immediately re-subscribe so the next
/// change arrives within one RTT.
///
/// Usage:
///   lp.watch('vpn.state', (v) => { this.vpnState = v; });
///   lp.watch('wifi.signal.rssi', (v) => { this.rssi = v; });
///   // On destroy:
///   lp.stopAll();
@Injectable({ providedIn: 'root' })
export class LongPollService implements OnDestroy {

  private watchers = new Map<string, Subscription>();

  constructor(private http: HttpsvcService) {}

  /// Watch a single key.  `cb` fires every time the value changes.
  /// Returns a subscription handle — call `.unsubscribe()` to stop.
  watch(key: string, cb: (value: unknown, changed: boolean) => void): Subscription {
    // Stop any existing watcher for this key
    this.unwatch(key);

    const poll = (): void => {
      const sub = this.http.dbGetLongPoll(key, 30).subscribe({
        next: (r) => {
          cb(r.value, r.changed);
          // Immediately re-poll
          poll();
        },
        error: () => {
          // On error (network down, server restart), retry after 5s
          setTimeout(() => poll(), 5000);
        }
      });
      this.watchers.set(key, sub);
    };

    poll();
    return this.watchers.get(key)!;
  }

  /// Stop watching a key.
  unwatch(key: string): void {
    const sub = this.watchers.get(key);
    if (sub) { sub.unsubscribe(); this.watchers.delete(key); }
  }

  /// Stop all watchers.
  stopAll(): void {
    for (const [key, sub] of this.watchers) {
      sub.unsubscribe();
    }
    this.watchers.clear();
  }

  ngOnDestroy(): void { this.stopAll(); }
}
