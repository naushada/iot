import { Injectable } from '@angular/core';
import { Subject } from 'rxjs';

export interface Toast {
  id: number;
  message: string;
  type: 'success' | 'error' | 'info';
}

@Injectable({ providedIn: 'root' })
export class ToastService {
  private nextId = 0;
  private _toasts = new Subject<Toast[]>();
  toasts: Toast[] = [];

  get toasts$() { return this._toasts.asObservable(); }

  success(msg: string): void { this.add(msg, 'success'); }
  error(msg: string): void { this.add(msg, 'error'); }
  info(msg: string): void { this.add(msg, 'info'); }

  private add(message: string, type: Toast['type']): void {
    const t: Toast = { id: this.nextId++, message, type };
    this.toasts.push(t);
    this._toasts.next(this.toasts);
    setTimeout(() => this.remove(t.id), 4000);
  }

  remove(id: number): void {
    this.toasts = this.toasts.filter(t => t.id !== id);
    this._toasts.next(this.toasts);
  }
}
