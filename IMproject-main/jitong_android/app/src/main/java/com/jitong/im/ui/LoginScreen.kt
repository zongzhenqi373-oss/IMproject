package com.jitong.im.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/** 登录/注册页：服务器地址默认 10.0.2.2（模拟器直连宿主 Mac im_server），界面不暴露 */
@Composable
fun LoginScreen(vm: MainViewModel) {
    val tip by vm.loginTip.collectAsStateWithLifecycle()
    var tab by rememberSaveable { mutableIntStateOf(0) }

    var nick by rememberSaveable { mutableStateOf("") }
    // 回填记住的账号密码（勾选“记住账号密码”后持久化在 MMKV）
    var tel by rememberSaveable { mutableStateOf(com.jitong.im.data.Prefs.tel.orEmpty()) }
    var pass by rememberSaveable { mutableStateOf(com.jitong.im.data.Prefs.pass.orEmpty()) }
    var remember by rememberSaveable { mutableStateOf(com.jitong.im.data.Prefs.remember) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text("即通", style = MaterialTheme.typography.headlineLarge, color = MaterialTheme.colorScheme.primary)
        Spacer(Modifier.height(24.dp))

        TabRow(selectedTabIndex = tab) {
            Tab(selected = tab == 0, onClick = { tab = 0 }, text = { Text("登录") })
            Tab(selected = tab == 1, onClick = { tab = 1 }, text = { Text("注册") })
        }
        Spacer(Modifier.height(16.dp))

        if (tab == 1) {
            OutlinedTextField(
                value = nick,
                onValueChange = { nick = it },
                label = { Text("昵称") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(12.dp))
        }
        OutlinedTextField(
            value = tel,
            onValueChange = { tel = it },
            label = { Text("手机号") },
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Phone),
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(12.dp))
        OutlinedTextField(
            value = pass,
            onValueChange = { pass = it },
            label = { Text("密码") },
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(20.dp))

        if (tab == 0) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { remember = !remember },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Checkbox(checked = remember, onCheckedChange = { remember = it })
                Text("记住账号密码")
            }
            Spacer(Modifier.height(8.dp))
        }

        Button(
            onClick = {
                if (tab == 0) vm.login(tel, pass, remember) else vm.register(nick, tel, pass)
            },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(if (tab == 0) "登录" else "注册")
        }
        Spacer(Modifier.height(12.dp))

        if (tip.isNotEmpty()) {
            Text(tip, color = MaterialTheme.colorScheme.secondary)
        }
    }
}
