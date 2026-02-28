import sys
import traceback
import numpy as np
import asyncio
import wave
from collections import deque
from pathlib import Path
import qasync

import matplotlib
matplotlib.use('qtagg')

from matplotlib.backends.backend_qtagg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

from PyQt6.QtWidgets import (
    QApplication,
    QMainWindow,
    QVBoxLayout,
    QWidget,
    QHBoxLayout,
    QLineEdit,
    QPushButton,
    QLabel,
    QTextEdit,
    QComboBox,
)
from PyQt6.QtCore import QTimer

from demod import RealTimeAFSKDecoder

LOG_PATH = Path('acoustic_check.log')


def log_message(message: str):
    print(message)
    try:
        with LOG_PATH.open('a', encoding='utf-8') as log_file:
            log_file.write(message + '\n')
    except Exception:
        pass


class UDPServerProtocol(asyncio.DatagramProtocol):
    def __init__(self, data_queue):
        self.client_address = None
        self.data_queue: deque = data_queue
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        if self.client_address is None:
            self.client_address = addr
            log_message(f'Accepted UDP stream from {addr}')

        if addr == self.client_address:
            self.data_queue.extend(data)
        else:
            log_message(f'Ignoring UDP data from unknown sender {addr}')


class MatplotlibWidget(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)

        self.figure = Figure()
        self.canvas = FigureCanvas(self.figure)

        layout = QVBoxLayout()
        layout.addWidget(self.canvas)
        self.setLayout(layout)

        self.freq = 16000
        self.channels = 2
        self.time_window = 20
        self.analysis_channel = 0
        self.wave_data = deque(maxlen=self.freq * self.time_window * self.channels * 2)
        self.signals = deque(maxlen=self.freq * self.time_window)
        self.error_callback = None

        self.ax1 = self.figure.add_subplot(2, 1, 1)
        self.ax2 = self.figure.add_subplot(2, 1, 2)

        self.ax1.set_title('Real-time Audio Waveform')
        self.ax1.set_xlabel('Sample Index')
        self.ax1.set_ylabel('Amplitude')
        self.line_time, = self.ax1.plot([], [])
        self.ax1.grid(True, alpha=0.3)

        self.ax2.set_title('Real-time Frequency Spectrum')
        self.ax2.set_xlabel('Frequency (Hz)')
        self.ax2.set_ylabel('Magnitude')
        self.line_freq, = self.ax2.plot([], [])
        self.ax2.grid(True, alpha=0.3)

        self.figure.tight_layout()

        self.timer = QTimer(self)
        self.timer.setInterval(100)
        self.timer.timeout.connect(self.update_plot)

        self.decoder = RealTimeAFSKDecoder(
            f_sample=self.freq,
            mark_freq=1800,
            space_freq=1500,
            bitrate=100,
            s_goertzel=9,
            threshold=0.5,
        )
        self.decode_callback = None

    def set_analysis_channel(self, channel: int):
        safe_channel = 0 if channel not in (0, 1) else channel
        self.analysis_channel = safe_channel
        self.signals.clear()
        self.wave_data.clear()
        if safe_channel == 0:
            self.decoder.clear()
        log_message(f'Switched analysis channel to {safe_channel}')

    def start_plotting(self):
        self.timer.start()

    def stop_plotting(self):
        self.timer.stop()

    def _report_error(self, exc: Exception):
        message = f'{type(exc).__name__}: {exc}'
        traceback_text = traceback.format_exc()
        log_message(message)
        log_message(traceback_text)
        if self.error_callback is not None:
            self.error_callback(message)

    def update_plot(self):
        try:
            self._update_plot_impl()
        except Exception as exc:
            self.stop_plotting()
            self._report_error(exc)

    def _update_plot_impl(self):
        frame_bytes = self.channels * 2
        if len(self.wave_data) >= frame_bytes:
            packet_len = len(self.wave_data) // frame_bytes * frame_bytes
            drained = [self.wave_data.popleft() for _ in range(packet_len)]
            samples = np.frombuffer(bytearray(drained), dtype='<i2')

            if self.channels > 1:
                samples = samples.reshape(-1, self.channels)
                channel_index = min(self.analysis_channel, self.channels - 1)
                signal = samples[:, channel_index].astype(np.float32) / 32768.0
            else:
                signal = samples.astype(np.float32) / 32768.0

            if self.analysis_channel == 0:
                decoded_text_new = self.decoder.process_audio(signal)
                if decoded_text_new and self.decode_callback:
                    self.decode_callback(decoded_text_new)

            self.signals.extend(signal.tolist())

        if len(self.signals) == 0:
            return

        signal = np.array(self.signals)
        max_samples = min(len(signal), self.freq * self.time_window)
        if len(signal) > max_samples:
            signal = signal[-max_samples:]

        x = np.arange(len(signal))
        self.line_time.set_data(x, signal)

        self.ax1.set_xlim(0, max(len(signal), 1))
        y_min, y_max = np.min(signal), np.max(signal)
        if y_min != y_max:
            margin = (y_max - y_min) * 0.1
            self.ax1.set_ylim(y_min - margin, y_max + margin)
        else:
            self.ax1.set_ylim(-1, 1)

        if len(signal) > 1:
            fft_signal = np.abs(np.fft.fft(signal))
            frequencies = np.fft.fftfreq(len(signal), 1 / self.freq)
            positive_freq_idx = frequencies >= 0
            freq_positive = frequencies[positive_freq_idx]
            fft_positive = fft_signal[positive_freq_idx]
            self.line_freq.set_data(freq_positive, fft_positive)

            if len(fft_positive) > 0:
                max_freq_show = min(4000, self.freq // 2)
                freq_mask = freq_positive <= max_freq_show
                if np.any(freq_mask):
                    self.ax2.set_xlim(0, max_freq_show)
                    fft_masked = fft_positive[freq_mask]
                    if len(fft_masked) > 0:
                        fft_max = np.max(fft_masked)
                        self.ax2.set_ylim(0, fft_max * 1.1 if fft_max > 0 else 1)

        self.canvas.draw_idle()


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('Acoustic Check')
        self.setGeometry(100, 100, 1000, 800)

        main_widget = QWidget()
        self.setCentralWidget(main_widget)
        main_layout = QVBoxLayout(main_widget)

        self.matplotlib_widget = MatplotlibWidget()
        self.matplotlib_widget.error_callback = self.on_plot_error
        main_layout.addWidget(self.matplotlib_widget)

        control_panel = QWidget()
        control_layout = QHBoxLayout(control_panel)

        control_layout.addWidget(QLabel('Listen address:'))
        self.address_input = QLineEdit('0.0.0.0')
        self.address_input.setFixedWidth(120)
        control_layout.addWidget(self.address_input)

        control_layout.addWidget(QLabel('Port:'))
        self.port_input = QLineEdit('8000')
        self.port_input.setFixedWidth(80)
        control_layout.addWidget(self.port_input)

        control_layout.addWidget(QLabel('Channel:'))
        self.channel_select = QComboBox()
        self.channel_select.addItems(['Mic', 'Reference'])
        self.channel_select.currentIndexChanged.connect(self.on_channel_changed)
        control_layout.addWidget(self.channel_select)

        self.listen_button = QPushButton('Start listening')
        self.listen_button.clicked.connect(self.toggle_listening)
        control_layout.addWidget(self.listen_button)

        self.status_label = QLabel('Status: idle')
        control_layout.addWidget(self.status_label)

        self.data_label = QLabel('Received data: 0 samples')
        control_layout.addWidget(self.data_label)

        self.save_button = QPushButton('Save audio')
        self.save_button.clicked.connect(self.save_audio)
        self.save_button.setEnabled(False)
        control_layout.addWidget(self.save_button)

        control_layout.addStretch()
        main_layout.addWidget(control_panel)

        decode_panel = QWidget()
        decode_layout = QVBoxLayout(decode_panel)

        decode_title = QLabel('Real-time AFSK decode:')
        decode_layout.addWidget(decode_title)

        self.decode_text = QTextEdit()
        self.decode_text.setMaximumHeight(150)
        self.decode_text.setReadOnly(True)
        self.decode_text.setStyleSheet("font-family: 'Courier New', monospace; font-size: 12px;")
        decode_layout.addWidget(self.decode_text)

        decode_control_layout = QHBoxLayout()
        self.clear_decode_button = QPushButton('Clear decode')
        self.clear_decode_button.clicked.connect(self.clear_decode_text)
        decode_control_layout.addWidget(self.clear_decode_button)

        self.decode_stats_label = QLabel('Decode stats: 0 bits, 0 chars')
        decode_control_layout.addWidget(self.decode_stats_label)
        decode_control_layout.addStretch()
        decode_layout.addLayout(decode_control_layout)

        main_layout.addWidget(decode_panel)

        self.matplotlib_widget.decode_callback = self.on_decode_text
        self.udp_transport = None
        self.is_listening = False

        self.stats_timer = QTimer(self)
        self.stats_timer.setInterval(1000)
        self.stats_timer.timeout.connect(self.update_stats)

    def on_plot_error(self, message: str):
        self.status_label.setText(f'Status: plot error - {message}')

    def on_channel_changed(self, index: int):
        channel = 0 if index <= 0 else 1
        self.matplotlib_widget.set_analysis_channel(channel)
        self.decode_text.clear()
        if channel == 0:
            self.decode_stats_label.setText('Decode stats: 0 bits, 0 chars')
            self.status_label.setText('Status: using Mic channel, AFSK decode enabled')
        else:
            self.decode_stats_label.setText('Decode stats: disabled on Reference channel')
            self.status_label.setText('Status: using Reference channel, AFSK decode disabled')

    def on_decode_text(self, new_text: str):
        if new_text:
            current_text = self.decode_text.toPlainText()
            updated_text = current_text + new_text
            if len(updated_text) > 1000:
                updated_text = updated_text[-1000:]
            self.decode_text.setPlainText(updated_text)
            cursor = self.decode_text.textCursor()
            cursor.movePosition(cursor.MoveOperation.End)
            self.decode_text.setTextCursor(cursor)

    def clear_decode_text(self):
        self.decode_text.clear()
        self.matplotlib_widget.decoder.clear()
        self.decode_stats_label.setText('Decode stats: 0 bits, 0 chars')

    def update_decode_stats(self):
        if self.matplotlib_widget.analysis_channel != 0:
            self.decode_stats_label.setText('Decode stats: disabled on Reference channel')
            return
        stats = self.matplotlib_widget.decoder.get_stats()
        stats_text = (
            f"Prelude: {stats['prelude_bits']}, chars: {stats['total_chars']}, "
            f"buffer: {stats['buffer_bits']} bits, state: {stats['state']}"
        )
        self.decode_stats_label.setText(stats_text)

    def toggle_listening(self):
        if not self.is_listening:
            self.start_listening()
        else:
            self.stop_listening()

    async def start_listening_async(self):
        try:
            address = self.address_input.text().strip()
            port = int(self.port_input.text().strip())

            loop = asyncio.get_running_loop()
            self.udp_transport, _ = await loop.create_datagram_endpoint(
                lambda: UDPServerProtocol(self.matplotlib_widget.wave_data),
                local_addr=(address, port),
            )

            self.status_label.setText(f'Status: listening on {address}:{port}')
            log_message(f'UDP server listening on {address}:{port}')
        except Exception as e:
            self.status_label.setText(f'Status: start failed - {e}')
            log_message(f'UDP server start failed: {e}')
            self.is_listening = False
            self.listen_button.setText('Start listening')
            self.address_input.setEnabled(True)
            self.port_input.setEnabled(True)

    def start_listening(self):
        try:
            int(self.port_input.text().strip())
        except ValueError:
            self.status_label.setText('Status: port must be numeric')
            return

        self.is_listening = True
        self.listen_button.setText('Stop listening')
        self.address_input.setEnabled(False)
        self.port_input.setEnabled(False)
        self.save_button.setEnabled(True)
        self.matplotlib_widget.wave_data.clear()
        self.matplotlib_widget.signals.clear()
        self.matplotlib_widget.start_plotting()
        self.stats_timer.start()

        loop = asyncio.get_event_loop()
        loop.create_task(self.start_listening_async())

    def stop_listening(self):
        self.is_listening = False
        self.listen_button.setText('Start listening')
        self.address_input.setEnabled(True)
        self.port_input.setEnabled(True)

        if self.udp_transport:
            self.udp_transport.close()
            self.udp_transport = None

        self.matplotlib_widget.stop_plotting()
        self.matplotlib_widget.wave_data.clear()
        self.stats_timer.stop()
        self.status_label.setText('Status: stopped')

    def update_stats(self):
        data_size = len(self.matplotlib_widget.signals)
        self.data_label.setText(f'Received data: {data_size} samples')
        self.update_decode_stats()

    def save_audio(self):
        if len(self.matplotlib_widget.signals) == 0:
            self.status_label.setText('Status: no data to save')
            return

        try:
            signal_data = np.array(self.matplotlib_widget.signals, dtype=np.float32)
            signal_data = signal_data - float(np.mean(signal_data))
            peak = float(np.max(np.abs(signal_data)))
            rms = float(np.sqrt(np.mean(np.square(signal_data))))
            if peak <= 1e-9:
                self.status_label.setText('Status: captured data is effectively silent')
                log_message('Save skipped: captured data is effectively silent')
                return

            normalized = signal_data / peak
            pcm_data = np.clip(normalized * 32767.0, -32768, 32767).astype(np.int16)

            with wave.open('received_audio.wav', 'wb') as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(self.matplotlib_widget.freq)
                wf.writeframes(pcm_data.tobytes())

            self.status_label.setText(f'Status: saved to received_audio.wav (peak={peak:.6f}, rms={rms:.6f})')
            log_message(f'Audio saved to received_audio.wav (peak={peak:.6f}, rms={rms:.6f})')
        except Exception as e:
            self.status_label.setText(f'Status: save failed - {e}')
            log_message(f'Failed to save audio: {e}')


async def main():
    LOG_PATH.write_text('', encoding='utf-8')
    log_message('Starting Acoustic Check GUI')

    app = QApplication(sys.argv)
    loop = qasync.QEventLoop(app)
    asyncio.set_event_loop(loop)

    window = MainWindow()
    window.show()

    try:
        with loop:
            await loop.run_forever()
    except KeyboardInterrupt:
        log_message('Interrupted by user')
    finally:
        if window.udp_transport:
            window.udp_transport.close()
