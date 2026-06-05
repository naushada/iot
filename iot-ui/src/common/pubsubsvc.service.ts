import { Injectable } from '@angular/core';
import { Subject, Subscription } from 'rxjs';
import { filter, map } from 'rxjs/operators';

/// Lightweight pub-sub for cross-component communication.
/// Components subscribe to named events; anything can publish.
/// Mirrors the xpmile PubSubService pattern exactly.
@Injectable({ providedIn: 'root' })
export class PubSubService {

  private subjects = new Map<string, Subject<unknown>>();

  private getSubject(name: string): Subject<unknown> {
    let s = this.subjects.get(name);
    if (!s) {
      s = new Subject<unknown>();
      this.subjects.set(name, s);
    }
    return s;
  }

  publish(name: string, payload?: unknown): void {
    this.getSubject(name).next(payload ?? null);
  }

  on<T = unknown>(name: string): import('rxjs').Observable<T> {
    return this.getSubject(name).asObservable().pipe(
      map(v => v as T)
    );
  }
}
