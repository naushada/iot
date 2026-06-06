import { Component, Input } from '@angular/core';

/// Reusable status badge — green/yellow/red/grey pill with a label.
/// Mirrors xpmile status-badge pattern.
@Component({
  selector: 'app-status-badge',
  template: `
    <span class="status-badge" [ngClass]="stateClass">
      <span class="status-dot"></span>
      {{ label }}
    </span>
  `,
  styles: [`
    .status-badge {
      display: inline-flex;
      align-items: center;
      gap: 5px;
      padding: 2px 10px;
      border-radius: 12px;
      font-size: 12px;
      font-weight: 500;
    }
    .status-dot {
      width: 7px; height: 7px;
      border-radius: 50%;
      display: inline-block;
    }
    .connected, .running {
      background: rgba(46,125,50,0.15); color: #2e7d32;
    }
    .connected .status-dot, .running .status-dot { background: #2e7d32; }
    .disconnected, .exited, .error {
      background: rgba(198,40,40,0.12); color: #c62828;
    }
    .disconnected .status-dot, .exited .status-dot, .error .status-dot { background: #c62828; }
    .starting, .connecting, .wait {
      background: rgba(249,168,37,0.15); color: #f57f17;
    }
    .starting .status-dot, .connecting .status-dot, .wait .status-dot { background: #f57f17; }
    .disabled {
      background: rgba(158,158,158,0.12); color: #757575;
    }
    .disabled .status-dot { background: #9e9e9e; }
  `]
})
export class StatusBadgeComponent {
  @Input() label = '';
  @Input() state = '';

  get stateClass(): string {
    const s = this.state.toLowerCase();
    if (s === 'running' || s === 'connected' || s === 'bound') return 'running';
    if (s === 'exited' || s === 'disconnected' || s === 'error' || s === 'conflict') return 'exited';
    if (s === 'starting' || s === 'connecting' || s === 'wait' ||
        s === 'associating' || s === 'requesting' || s === 'monitoring') return 'starting';
    if (s === 'disabled' || s === 'idle' || s === 'stopped') return 'disabled';
    return '';
  }
}
