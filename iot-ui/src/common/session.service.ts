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

  clear(): void { this.session = null; }

  /// Probe /api/v1/status — 401 when unauthenticated.
  loadSession(): Observable<SessionInfo | null> {
    return this.http.getStatus().pipe(
      map(() => {
        this.session = { role: 'admin', access: 'Admin' };
        return this.session;
      }),
      catchError(() => { this.session = null; return of(null); })
    );
  }

  /// Cache session after login with the access level from the server.
  setFromLogin(access?: string): void {
    this.session = { role: 'admin', access: access || 'Viewer' };
  }
}

export function initSession(session: SessionService): () => Promise<unknown> {
  return () => firstValueFrom(session.loadSession());
}
