using System;
using System.IO.Ports;
using System.Windows.Forms;
using System.Windows.Forms.DataVisualization.Charting;
using System.Collections.Concurrent;

namespace Oscilloscope_STM32_Screen
{
    public partial class Form1 : Form
    {
        private const int MAX_SAMPLES = 20000; // Number of points on screen
        private byte[] frameBuffer = new byte[14];
        private int bufferIndex = 0;
        private int expectedLength = 0;
        private byte activeHeader = 0;
        private volatile bool isDisconnecting = false;
        private ConcurrentQueue<(double ch1,double ch2)> sampleQueue = new ConcurrentQueue<(double ch1,double ch2)>();
        private System.Windows.Forms.Timer renderTimer;

        public Form1()
        {
            InitializeComponent();
            SetupChart();
            PopulateSerialPorts();
            renderTimer = new System.Windows.Forms.Timer();
            renderTimer.Interval = 33; // Update UI roughly 30 times a second
            renderTimer.Tick += RenderTimer_Tick;
            renderTimer.Start();
        }

        private void SetupChart()
        {
            chart1.Series.Clear();
            Series series1 = new Series("Channel 1")
            {
                ChartType = SeriesChartType.FastLine,
                Color = System.Drawing.Color.Lime,
                BorderWidth = 2
            };
            chart1.Series.Add(series1);

            Series series2 = new Series("Channel 2")
            {
                ChartType = SeriesChartType.FastLine,
                Color = System.Drawing.Color.Cyan,
                BorderWidth = 2
            };
            chart1.Series.Add(series2);

            for (int i = 0; i < MAX_SAMPLES; i++)
            {
                series2.Points.AddY(0); // Pre-fill CH2
            }

            ChartArea area = chart1.ChartAreas[0];
            area.BackColor = System.Drawing.Color.Black;
            area.AxisY.Minimum = -4.0;
            area.AxisY.Maximum = 4.0; // Volts scale
            area.AxisY.Title = "Voltage (V)";
            area.AxisX.Title = "Samples";
            area.AxisX.MajorGrid.LineColor = System.Drawing.Color.DarkGreen;
            area.AxisY.MajorGrid.LineColor = System.Drawing.Color.DarkGreen;


            // Y-Axis: Major grid every 0.5V, Minor grid every 0.1V
            area.AxisY.Interval = 0.5;
            area.AxisY.MinorGrid.Enabled = true;
            area.AxisY.MinorGrid.Interval = 0.1;
            area.AxisY.MinorGrid.LineColor = System.Drawing.Color.FromArgb(40, 0, 100, 0); // Faint dark green

            // X-Axis: Major grid every 200 samples, Minor grid every 50 samples
            area.AxisX.Interval = 200;
            area.AxisX.MinorGrid.Enabled = true;
            area.AxisX.MinorGrid.Interval = 50;
            area.AxisX.MinorGrid.LineColor = System.Drawing.Color.FromArgb(40, 0, 100, 0);

            // Allow user to click and drag a box to zoom horizontally
            area.CursorX.IsUserEnabled = true;
            area.CursorX.IsUserSelectionEnabled = true;
            area.AxisX.ScaleView.Zoomable = true;
            area.AxisX.ScrollBar.IsPositionedInside = true;

            // Allow user to click and drag a box to zoom vertically
            area.CursorY.IsUserEnabled = true;
            area.CursorY.IsUserSelectionEnabled = true;
            area.AxisY.ScaleView.Zoomable = true;
            area.AxisY.ScrollBar.IsPositionedInside = true;



            // Pre-fill CH1 with zeros
            for (int i = 0; i < MAX_SAMPLES; i++)
            {
                series1.Points.AddY(0);
            }
        }

        private void PopulateSerialPorts()
        {
            comboBoxPorts.Items.Clear();
            string[] ports = SerialPort.GetPortNames();
            comboBoxPorts.Items.AddRange(ports);
            if (ports.Length > 0) comboBoxPorts.SelectedIndex = 0;
        }

        private void btnConnect_Click(object sender, EventArgs e)
        {
            if (!serialPort1.IsOpen)    // Serial port closed means it is not communicating with the external hardware
            {
                try
                {
                    serialPort1.PortName = comboBoxPorts.SelectedItem.ToString();
                    serialPort1.BaudRate = 921600;
                    serialPort1.DataReceived += SerialPort1_DataReceived;
                    serialPort1.Open();     // Serial port open means it is communicating with external hardware
                    btnConnect.Text = "Disconnect";
                }
                catch (Exception ex)
                {
                    MessageBox.Show("Error opening serial port: " + ex.Message);
                }
            }
            else
            {
                isDisconnecting = true; 

                // Force the UI thread to pause for 50 milliseconds. 
                // This gives the background thread enough time to finish its current loop and hit the 'return' statement.
                System.Threading.Thread.Sleep(50);

                try
                {
                    serialPort1.DataReceived -= SerialPort1_DataReceived;
                    serialPort1.DiscardInBuffer();
                    serialPort1.Close();
                }
                catch (Exception)
                {
                    // Silently swallow any lingering driver-level OS exceptions
                }

                isDisconnecting = false; // Reset the flag for the next connection
                btnConnect.Text = "Connect";
            }
        }

        private void SerialPort1_DataReceived(object sender, SerialDataReceivedEventArgs e)
        {
            if (isDisconnecting) return;

            int bytesToRead = serialPort1.BytesToRead;
            for (int i = 0; i < bytesToRead; i++)
            {
                byte b = (byte)serialPort1.ReadByte();

                if (bufferIndex == 0)
                {
                    if (b == 0xAA) { activeHeader = 0xAA; expectedLength = 4; frameBuffer[0] = b; bufferIndex = 1; }
                    else if (b == 0xBB) { activeHeader = 0xBB; expectedLength = 14; frameBuffer[0] = b; bufferIndex = 1; }
                    else if (b == 0xFF) { activeHeader = 0xFF; expectedLength = 4; frameBuffer[0] = b; bufferIndex = 1; }
                }
                else
                {
                    frameBuffer[bufferIndex] = b;
                    bufferIndex++;

                    if (bufferIndex == expectedLength)
                    {
                        bufferIndex = 0; // Reset for next frame

                        // Validate tail byte (0x55) to prevent misaligned execution
                        if (frameBuffer[expectedLength - 1] == 0x55)
                        {
                            if (activeHeader == 0xAA) // Raw ADC Data Packet
                            {
                                double v1 = ((frameBuffer[1] / 255.0) * 6.6) - 3.3;
                                double v2 = ((frameBuffer[2] / 255.0) * 6.6) - 3.3;
                                sampleQueue.Enqueue((v1, v2));
                            }
                            else if (activeHeader == 0xBB) // Statistics Packet
                            {
                                double maxV1 = ((frameBuffer[1] / 255.0) * 6.6) - 3.3;
                                double minV1 = ((frameBuffer[2] / 255.0) * 6.6) - 3.3;
                                int freq1 = (frameBuffer[3] << 8) | frameBuffer[4];
                                double rmsV1 = ((frameBuffer[5] << 8) | frameBuffer[6]) / 100.0;
                                double vpp1 = maxV1 - minV1;

                                double maxV2 = ((frameBuffer[7] / 255.0) * 6.6) - 3.3;
                                double minV2 = ((frameBuffer[8] / 255.0) * 6.6) - 3.3;
                                int freq2 = (frameBuffer[9] << 8) | frameBuffer[10];
                                double rmsV2 = ((frameBuffer[11] << 8) | frameBuffer[12]) / 100.0;
                                double vpp2 = maxV2 - minV2;

                                if (vpp1 < 0.2) freq1 = 0; // Noise gate
                                if (vpp2 < 0.2) freq2 = 0;

                                BeginInvoke(new Action(() => {
                                    lblStats1.Text = $"Channel 1:\nVmax: {maxV1:F2} V\nVmin: {minV1:F2} V\nVpp: {vpp1:F2} V\nVrms: {rmsV1:F2} V\nFreq: {freq1} Hz";
                                    lblStats2.Text = $"Channel 2:\nVmax: {maxV2:F2} V\nVmin: {minV2:F2} V\nVpp: {vpp2:F2} V\nVrms: {rmsV2:F2} V\nFreq: {freq2} Hz";
                                }));
                            }
                            else if (activeHeader == 0xFF) // Command Frame
                            {
                                byte cmd = frameBuffer[1];
                                byte val = frameBuffer[2];

                                if (cmd == 0xC1) // Pause Status Command
                                {
                                    BeginInvoke(new Action(() => lblHardwareStatus.Text = (val == 0x01) ? "Status: PAUSED" : "Status: RUNNING"));
                                }
                                else if (cmd == 0xE1) // CH1 Toggle Command
                                {
                                    BeginInvoke(new Action(() => chart1.Series["Channel 1"].Enabled = (val == 0x01)));
                                }
                                else if (cmd == 0xF1) // CH2 Toggle Command
                                {
                                    BeginInvoke(new Action(() => chart1.Series["Channel 2"].Enabled = (val == 0x01)));
                                }
                            }
                        }
                    }
                }
            }
        }

        private void RenderTimer_Tick(object sender, EventArgs e)
        {
            if (sampleQueue.IsEmpty) return;

            Series series1 = chart1.Series["Channel 1"];
            Series series2 = chart1.Series["Channel 2"];

            // Suspend updates to prevent the chart from rendering during the loop (gives a massive performance boost)
            chart1.Series.SuspendUpdates();

            // Dump all pending samples from the background thread onto the chart
            while (sampleQueue.TryDequeue(out var voltages))
            {
                series1.Points.AddY(voltages.ch1);
                series2.Points.AddY(voltages.ch2);
            }

            // Trim the window to MAX_SAMPLES
            while (series1.Points.Count > MAX_SAMPLES)
            {
                series1.Points.RemoveAt(0);
                series2.Points.RemoveAt(0);
            }

            // Resume updates to draw the entire batch at once
            chart1.Series.ResumeUpdates();

        }


        private void Form1_FormClosing(object sender, FormClosingEventArgs e)
        {
            if (serialPort1.IsOpen)
            {
                isDisconnecting = true; 

                // Force the UI thread to pause for 50 milliseconds. 
                // This gives the background thread enough time to finish its current loop and hit the 'return' statement.
                System.Threading.Thread.Sleep(50);

                try
                {
                    serialPort1.DataReceived -= SerialPort1_DataReceived;
                    serialPort1.DiscardInBuffer();
                    serialPort1.Close();
                }
                catch (Exception)
                {
                    // Silently swallow any lingering driver-level OS exceptions
                }

                isDisconnecting = false; // Reset the flag for the next connection
            }
        }
    }
}