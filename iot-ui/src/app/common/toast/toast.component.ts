import { Component } from '@angular/core';
import { ToastService, Toast } from '../../../common/toast.service';

@Component({
  selector: 'app-toast',
  template: `
    <div class="toast-container">
      <div class="toast" *ngFor="let t of toasts"
           [class.success]="t.type==='success'"
           [class.error]="t.type==='error'"
           [class.info]="t.type==='info'"
           (click)="svc.remove(t.id)">
        <clr-icon [attr.shape]="t.type==='success'?'check-circle':t.type==='error'?'exclamation-circle':'info-circle'"></clr-icon>
        <span>{{ t.message }}</span>
      </div>
    </div>
  `,
  styles: [`
    .toast-container {
      position: fixed; bottom: 20px; right: 20px; z-index: 9999;
      display: flex; flex-direction: column-reverse; gap: 8px;
    }
    .toast {
      display: flex; align-items: center; gap: 10px;
      padding: 12px 20px; border-radius: 6px; cursor: pointer;
      font-size: 13px; min-width: 280px; max-width: 420px;
      box-shadow: 0 4px 12px rgba(0,0,0,0.15); animation: slideIn 0.2s ease;
    }
    .success { background: #e8f5e9; color: #2e7d32; border: 1px solid #a5d6a7; }
    .error   { background: #ffebee; color: #c62828; border: 1px solid #ef9a9a; }
    .info    { background: #e3f2fd; color: #1565c0; border: 1px solid #90caf9; }
    @keyframes slideIn { from { transform: translateX(100%); opacity: 0; } to { transform: translateX(0); opacity: 1; } }
  `]
})
export class ToastComponent {
  toasts: Toast[] = [];
  constructor(public svc: ToastService) {
    svc.toasts$.subscribe(t => this.toasts = t);
  }
}
