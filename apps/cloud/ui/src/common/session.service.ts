import { Injectable } from '@angular/core';
import { firstValueFrom, Observable, of } from 'rxjs';
import { catchError, map } from 'rxjs/operators';
import { HttpsvcService } from './httpsvc.service';
import { SessionInfo } from './app-globals';

/// Holds the current auth session for the SPA.
@Injectable({ providedIn: 'root' })
export class SessionService {

  private session: SessionInfo | null = null;

  constructor(private http: HttpsvcService) {}

  get isAuthenticated(): boolean { return this.session !== null; }
  get role(): string | undefined { return this.session?.role; }
  get access(): string { return this.session?.access || 'Viewer'; }
  get isAdmin(): boolean { return this.access === 'Admin'; }
  /// The session's owning tenant ("*" = platform operator, sees all tenants).
  get tenant(): string { return this.session?.tenant || 'default'; }
  /// Platform operator — the only role that manages tenants / sees every tenant.
  get isPlatformOperator(): boolean { return this.tenant === '*'; }

  clear(): void { this.session = null; localStorage.removeItem('iot-session'); }

  /// Probe /api/v1/status — 401 when unauthenticated. The tenant/access aren't
  /// in /status, so restore them from the login-persisted copy (falls back to
  /// platform-operator admin, the legacy single-tenant assumption).
  loadSession(): Observable<SessionInfo | null> {
    return this.http.getStatus().pipe(
      map(() => {
        this.session = this.restore() || { role: 'admin', access: 'Admin', tenant: '*' };
        return this.session;
      }),
      catchError(() => { this.session = null; return of(null); })
    );
  }

  /// Cache + persist session after login (access + owning tenant from server).
  setFromLogin(access?: string, tenant?: string): void {
    this.session = { role: 'admin', access: access || 'Viewer', tenant: tenant || 'default' };
    try { localStorage.setItem('iot-session', JSON.stringify(this.session)); } catch { /* noop */ }
  }

  private restore(): SessionInfo | null {
    try {
      const s = JSON.parse(localStorage.getItem('iot-session') || 'null');
      return (s && s.access && s.tenant) ? s as SessionInfo : null;
    } catch { return null; }
  }
}

export function initSession(session: SessionService): () => Promise<unknown> {
  return () => firstValueFrom(session.loadSession());
}
